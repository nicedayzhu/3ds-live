package io.github.nicedayzhu.three_ds_live.stream

import android.content.Context
import android.os.SystemClock
import io.flutter.view.TextureRegistry
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.random.Random

/**
 * Owns the receive pipeline: discovery, media reassembly, row/delta reconstruction,
 * audio playout, configuration, feedback and the telemetry window.
 */
class StreamEngine(
    private val context: Context,
    registry: TextureRegistry,
    private val events: (Map<String, Any?>) -> Unit,
) {
    data class Settings(
        var port: Int = 8001,
        var handheldIp: String = "",
        var autoDiscover: Boolean = true,
        var fps: Int = 30,
        var rate: Int = 800000,
        var quality: Int = 0,
        var adaptive: Boolean = true,
        var bufferMs: Int = 100,
        var layout: String = "stacked",
        var muted: Boolean = false,
        var volume: Float = 1f,
        var recordResolution: String = "720p",
        var recordBitrate: Int = 8_000_000,
    ) {
        companion object {
            fun fromMap(map: Map<*, *>?): Settings {
                val settings = Settings()
                if (map == null) return settings
                (map["port"] as? Number)?.let { settings.port = it.toInt() }
                (map["handheld_ip"] as? String)?.let { settings.handheldIp = it }
                (map["auto_discover"] as? Boolean)?.let { settings.autoDiscover = it }
                (map["fps"] as? Number)?.let { settings.fps = it.toInt() }
                (map["rate"] as? Number)?.let { settings.rate = it.toInt() }
                (map["jpeg_quality"] as? Number)?.let { settings.quality = it.toInt() }
                (map["adaptive_rate"] as? Boolean)?.let { settings.adaptive = it }
                (map["buffer_ms"] as? Number)?.let { settings.bufferMs = it.toInt() }
                (map["layout"] as? String)?.let { settings.layout = it }
                (map["muted"] as? Boolean)?.let { settings.muted = it }
                (map["volume"] as? Number)?.let { settings.volume = it.toFloat() }
                (map["record_resolution"] as? String)?.let { settings.recordResolution = it }
                (map["record_bitrate"] as? Number)?.let { settings.recordBitrate = it.toInt() }
                return settings
            }
        }
    }

    private val sink = VideoSink(context, registry) { quality, _, _ ->
        lastQuality = quality
    }
    private val audio = AudioPlayer()
    private val recorder = Recorder(context)
    private val running = AtomicBoolean(false)

    private var socket: DatagramSocket? = null
    private var receiveThread: Thread? = null
    private var tickThread: Thread? = null

    var settings = Settings()
        private set

    @Volatile private var phase = "idle"
    @Volatile private var message: String? = null

    private var peerIp: String? = null
    private var peerPort = 0
    private var session = 0
    private var lastTraffic = 0L
    private var lastPeerSeen = 0L
    private var challenge = 0
    private var previousChallenge = 0
    private var sessionStarted = 0L

    private val resolver = ScreenResolver()
    private var peerSupportsQuality = false
    private var peerSupportsRepair = 0
    private var peerSupportsDictionary = false
    private var peerSupportsRows = false
    private var appliedQuality = 0
    private var configuredAt = 0L
    private var lastConfigNanos = 0L
    private var lastFeedbackNanos = 0L
    private var lastAckNanos = 0L
    private var lastInviteNanos = 0L

    private var configSeq = 1
    private var capabilitiesAt = 0L

    private val assembler = Assembler(
        onComplete = ::onPair,
        onIncomplete = { seq, missing ->
            statsIncomplete++
            statsDropped += missing
            emit("stats", mapOf("events" to listOf("帧 $seq 缺 $missing 片")))
        },
        onNeedRepair = ::requestRepair,
    )

    // diagnostics counters
    private var packets = 0L
    private var pairs = 0L
    private var incompletePairs = 0L
    private var repairedPairs = 0L
    private var repairRequests = 0L
    private var repairPackets = 0L
    private var repairTimeouts = 0L
    private var statsIncomplete = 0L
    private var statsDropped = 0L
    private var lastQuality = 0
    private var audioBlocks = 0L
    private var audioDropped = 0L

    // one-second window
    private var windowStart = 0L
    private var windowPairs = 0L
    private var windowPackets = 0L
    private var windowBytes = 0L
    private var windowSentBytes = 0L
    private var windowStarted = 0L
    private var windowRowsSaved = 0L
    private var windowRowsTotal = 0L
    private var windowLz4Saved = 0L
    private var windowLz4Total = 0L
    private var windowAudioGaps = 0L
    private var lastMediaBytes = -1L
    private var lastMediaAt = 0L

    private var cap = 0
    private val rate = RateController()
    private var deviceSentBytes = 0L
    private var deviceAppliedQuality = 0
    private var lastStatusNanos = 0L

    private val seenAudio = HashSet<Int>()
    private val deviceQualitySeen = HashSet<Int>()

    val textureId: Long get() = sink.textureId

    fun start(settings: Settings) {
        if (!running.compareAndSet(false, true)) {
            applySettings(settings)
            return
        }
        this.settings = settings
        rate.reset(settings.rate)
        sink.setLayout(VideoSink.Layout.from(settings.layout))
        audio.muted = settings.muted
        audio.volume = settings.volume
        audio.start()
        windowStart = SystemClock.elapsedRealtimeNanos()
        windowStarted = windowStart
        configuredAt = 0
        try {
            val datagramSocket = DatagramSocket(null)
            datagramSocket.reuseAddress = true
            datagramSocket.receiveBufferSize = 4 * 1024 * 1024
            datagramSocket.bind(InetSocketAddress(settings.port))
            datagramSocket.soTimeout = 30
            socket = datagramSocket
        } catch (error: Throwable) {
            running.set(false)
            audio.stop()
            setPhase("error", "无法监听 UDP ${settings.port}：${error.message}")
            return
        }
        setPhase("inviting", null)
        receiveThread = Thread({ receiveLoop() }, "3ds-live-rx").apply { start() }
        tickThread = Thread({ tickLoop() }, "3ds-live-tick").apply { start() }
    }

    fun stop() {
        stopRecording()
        if (!running.getAndSet(false)) return
        receiveThread?.join(300)
        tickThread?.join(300)
        receiveThread = null
        tickThread = null
        socket?.close()
        socket = null
        audio.stop()
        sink.clear()
        peerIp = null
        peerPort = 0
        session = 0
        resolver.reset()
        assembler.reset()
        setPhase("idle", null)
    }

    fun applySettings(next: Settings) {
        val layoutChanged = next.layout != settings.layout
        val targetChanged = next.handheldIp != settings.handheldIp
        settings = next
        rate.reset(next.rate)
        audio.muted = next.muted
        audio.volume = next.volume
        if (layoutChanged) sink.setLayout(VideoSink.Layout.from(next.layout))
        if (running.get()) {
            if (targetChanged) {
                peerIp = next.handheldIp.ifEmpty { null }
                session = 0
                resolver.reset()
                assembler.reset()
                setPhase("inviting", null)
            } else if (peerIp != null) {
                sendConfiguration(force = true)
            }
        }
    }

    fun setLayout(layout: String) {
        settings.layout = layout
        sink.setLayout(VideoSink.Layout.from(layout))
    }

    fun capture(): String? = sink.capture()

    /** Starts MP4 recording of the composed canvas plus the decoded audio. */
    fun startRecording(): String? {
        if (recorder.isRecording) return recorder.path
        val scale = if (settings.recordResolution == "1080p") 1.5 else 1.0
        val width = even(sink.layout.width * scale)
        val height = even(sink.layout.height * scale)
        val path = recorder.start(width, height, settings.recordBitrate, withAudio = !settings.muted)
        if (path != null) {
            sink.setRecordSurface(recorder.surface)
            audio.pcmSink = { pcm -> recorder.writeAudio(pcm) }
            events(mapOf("type" to "recording", "active" to true, "path" to path))
        }
        return path
    }

    fun stopRecording(): String? {
        val wasRecording = recorder.isRecording
        val path = recorder.stop()
        sink.setRecordSurface(null)
        audio.pcmSink = null
        if (wasRecording) {
            events(mapOf("type" to "recording", "active" to false, "path" to path))
        }
        return path
    }

    private fun even(value: Double): Int {
        val rounded = value.toInt()
        return if (rounded % 2 == 0) rounded else rounded + 1
    }

    fun refreshTexture(): Long = sink.textureId
    fun release() {
        stop()
        sink.release()
    }

    // ---------------------------------------------------------------- receive

    private fun receiveLoop() {
        val buffer = ByteArray(2048)
        val packet = DatagramPacket(buffer, buffer.size)
        while (running.get()) {
            val datagramSocket = socket ?: return
            try {
                packet.setLength(buffer.size)
                datagramSocket.receive(packet)
            } catch (_: SocketTimeoutException) {
                continue
            } catch (_: Throwable) {
                return
            }
            val now = SystemClock.elapsedRealtimeNanos()
            try {
                handleDatagram(packet, now)
            } catch (_: Throwable) {
                // A malformed datagram must never kill the receive loop.
            }
        }
    }

    private fun handleDatagram(packet: DatagramPacket, now: Long) {
        val length = packet.length
        val data = packet.data
        val tag = Wire.tag(data, length)
        val address = packet.address?.hostAddress ?: return
        when (tag) {
            Wire.DISCOVERY_PROBE_REPLY -> onProbeReply(address, packet.port, data, length, now)
            Wire.R10P, Wire.R10F, Wire.R19P -> onMedia(data, length, address, packet.port, now, tag)
            Wire.R3P2, Wire.R3F2 -> onPerScreen(data, length, address, packet.port, now, tag)
            Wire.R9A1 -> onAudio(data, length, address, now)
            Wire.R7ST, Wire.R9ST -> onStatus(data, length, address, packet.port, now, tag)
            Wire.R16S -> onQualityReport(data, length, address, now)
            Wire.R16Q -> onQualityCapability(data, length, address)
            Wire.R17S -> onRowsOffer(data, length, address)
            Wire.R19S -> onRepairOffer(data, length, address)
            Wire.R20S -> onDictionaryOffer(data, length, address)
            Wire.R17T -> onByteReport(data, length, address)
            Wire.R19T -> onRepairReport(data, length, address)
        }
    }

    private fun accepts(address: String, datagramSession: Int? = null): Boolean {
        val expected = peerIp
        if (expected != null && expected != address) return false
        if (datagramSession != null && session != 0 && datagramSession != session) return false
        return true
    }

    private fun onProbeReply(address: String, port: Int, data: ByteArray, length: Int, now: Long) {
        if (length != 12) return
        val replySession = Wire.int(data, 4)
        val replyChallenge = Wire.int(data, 8)
        val selected = peerIp
        if (selected != null && selected != address) return
        if (replyChallenge != challenge && replyChallenge != previousChallenge) return
        if (session != 0 && replySession != session && phase == "live") return
        peerIp = address
        peerPort = port
        if (replySession != session) {
            session = replySession
            sessionStarted = now
            onNewSession()
        }
        lastPeerSeen = now
        sendBinary(Wire.message(Wire.DISCOVERY_INVITE_REPLY, replySession, replyChallenge), address, port)
        if (phase == "inviting" || phase == "lost") setPhase("reserved", null)
        emitDevices()
    }

    private fun onNewSession() {
        assembler.reset()
        resolver.reset()
        appliedQuality = 0
        deviceAppliedQuality = 0
        deviceQualitySeen.clear()
        seenAudio.clear()
        lastMediaBytes = -1
        configuredAt = 0
        lastConfigNanos = 0
    }

    private fun onMedia(
        data: ByteArray,
        length: Int,
        address: String,
        port: Int,
        now: Long,
        tag: String,
    ) {
        if (!accepts(address, Wire.int(data, 4))) return
        peerPort = port
        peerIp = address
        lastPeerSeen = now
        lastTraffic = now
        packets++
        windowPackets++
        windowBytes += length + 28
        if (tag == Wire.R19P) repairPackets++
        if (assembler.submit(data, length, now)) {
            if (phase == "reserved" || phase == "inviting") setPhase("live", null)
        }
    }

    private fun onPerScreen(data: ByteArray, length: Int, address: String, port: Int, now: Long, tag: String) {
        if (!accepts(address, Wire.int(data, 4))) return
        lastTraffic = now
        peerIp = address
        peerPort = port
        lastPeerSeen = now
        packets++
        windowPackets++
        windowBytes += length + 28
        if (assembler.submitPerScreen(data, length, now) && phase == "reserved") {
            setPhase("live", null)
        }
    }

    private fun onAudio(data: ByteArray, length: Int, address: String, now: Long) {
        if (length < Wire.HEADER_R9A1) return
        if (!accepts(address, Wire.int(data, 4))) return
        val count = Wire.u16(data, 12)
        if (count !in 1..6) return
        if (length < Wire.HEADER_R9A1 + count * Wire.RECORD_R9A1) return
        lastTraffic = now
        windowBytes += length + 28
        var offset = Wire.HEADER_R9A1
        for (i in 0 until count) {
            val block = Wire.int(data, offset)
            if (seenAudio.add(block)) {
                audio.submitRecord(data, offset + 8, Wire.RECORD_R9A1 - 8, now)
                audioBlocks++
            }
            offset += Wire.RECORD_R9A1
        }
        if (seenAudio.size > 400) {
            val iterator = seenAudio.iterator()
            repeat(100) { if (iterator.hasNext()) { iterator.next(); iterator.remove() } }
        }
    }

    private fun onStatus(data: ByteArray, length: Int, address: String, port: Int, now: Long, tag: String) {
        val minimum = if (tag == Wire.R9ST) 108 else 76
        if (length < minimum) return
        val statusSession = Wire.int(data, 4)
        if (!accepts(address, statusSession)) return
        if (session != 0 && statusSession != session) return
        peerIp = address
        peerPort = port
        lastPeerSeen = now
        cap = Wire.int(data, 12).coerceIn(400000, 2000000)
        // R7ST words after tag: 0 session,1 elapsed,2 cap,3 videoBudget,4 fps,
        // 5 encTop,6 encBottom,7 sentTop,8 sentBottom,...
        val sentTop = Wire.u32(data, 4 + 7 * 4)
        val sentBottom = Wire.u32(data, 4 + 8 * 4)
        deviceSentBytes = sentTop + sentBottom
        lastStatusNanos = now
        if (phase == "reserved") setPhase("live", null)
    }

    private fun onQualityReport(data: ByteArray, length: Int, address: String, now: Long) {
        if (length < 20) return
        val statusSession = Wire.int(data, 4)
        if (!accepts(address, statusSession)) return
        peerSupportsQuality = true
        appliedQuality = Wire.int(data, 12)
        lastStatusNanos = now
    }

    private fun onQualityCapability(data: ByteArray, length: Int, address: String) {
        if (length < 16) return
        if (!accepts(address, Wire.int(data, 4))) return
        peerSupportsQuality = true
        deviceQualitySeen.add(Wire.int(data, 12))
    }

    private fun onRowsOffer(data: ByteArray, length: Int, address: String) {
        if (length < 12) return
        val offeredSession = Wire.int(data, 4)
        if (!accepts(address, offeredSession)) return
        peerSupportsRows = true
        val ref = resolver.referenceSeq
        sendBinary(Wire.message(Wire.R17A, session, if (ref != null) 1 else 0, ref ?: 0), address, peerPort)
    }

    private fun onRepairOffer(data: ByteArray, length: Int, address: String) {
        if (length < 16) return
        val offeredSession = Wire.int(data, 4)
        if (!accepts(address, offeredSession)) return
        peerSupportsRepair = Wire.int(data, 12).coerceIn(0, 16)
        sendBinary(Wire.message(Wire.R19A, session, 1), address, peerPort)
    }

    private fun onDictionaryOffer(data: ByteArray, length: Int, address: String) {
        if (length < 12) return
        val offeredSession = Wire.int(data, 4)
        if (!accepts(address, offeredSession)) return
        peerSupportsDictionary = true
        resolver.setDictionarySupported(true)
        sendBinary(Wire.message(Wire.R20A, session, 1), address, peerPort)
    }

    private fun onByteReport(data: ByteArray, length: Int, address: String) {
        if (length < 20) return
        val reportSession = Wire.int(data, 4)
        if (!accepts(address, reportSession)) return
        val bytes = Wire.u32(data, 12)
        val now = SystemClock.elapsedRealtimeNanos()
        if (lastMediaBytes >= 0 && lastMediaAt > 0) {
            val delta = bytes - lastMediaBytes
            if (delta >= 0) windowSentBytes += delta
        }
        lastMediaBytes = bytes
        lastMediaAt = now
    }

    private fun onRepairReport(data: ByteArray, length: Int, address: String) {
        if (length < 44) return
        if (!accepts(address, Wire.int(data, 4))) return
        // word 4: accepted repair requests, word 5: submitted repair data packets.
        repairRequests = maxOf(repairRequests, Wire.u32(data, 20))
        repairPackets = maxOf(repairPackets, Wire.u32(data, 24))
    }

    // ---------------------------------------------------------------- pairs

    private fun onPair(pair: Assembler.Pair) {
        pairs++
        windowPairs++
        val top = resolver.resolve(pair.top, 0)
        val bottom = resolver.resolve(pair.bottom, 1)
        if (resolver.completePair(pair.seq, top, bottom)) {
            peerIp?.let { address ->
                sendBinary(Wire.message(Wire.R17A, session, 1, pair.seq), address, peerPort)
            }
        }
        if (resolver.consumeFreshKeyRequest()) requestFreshKey()
        if (top == null && bottom == null) return
        val topBitmap = top?.let { sink.decode(it.jpeg) }
        val bottomBitmap = bottom?.let { sink.decode(it.jpeg) }
        if (topBitmap == null && bottomBitmap == null) return
        sink.present(topBitmap, bottomBitmap, top?.quality ?: 0, bottom?.quality ?: 0)
        if (pair.usedRepair) repairedPairs++
        recordRowSavings(pair, top, bottom)
        if (phase != "live") setPhase("live", null)
    }

    private fun recordRowSavings(pair: Assembler.Pair, top: ScreenResolver.Resolved?, bottom: ScreenResolver.Resolved?) {
        if (top != null && !top.isKey && top.parsed == null) {
            val reference = resolver.referenceSize(0)
            if (reference > 0) {
                windowRowsTotal += 1
                windowRowsSaved += (reference - pair.top.size).coerceAtLeast(0).toLong()
            }
        }
        if (bottom != null && !bottom.isKey && bottom.parsed == null) {
            val reference = resolver.referenceSize(1)
            if (reference > 0) {
                windowRowsTotal += 1
                windowRowsSaved += (reference - pair.bottom.size).coerceAtLeast(0).toLong()
            }
        }
    }

    private fun requestFreshKey() {
        if (!peerSupportsRows) return
        val now = SystemClock.elapsedRealtimeNanos()
        if (now - lastAckNanos < 250_000_000L) return
        lastAckNanos = now
        val address = peerIp ?: return
        sendBinary(Wire.message(Wire.R17A, session, 0, 0), address, peerPort)
    }

    private fun requestRepair(seq: Int, pts: Long, indices: IntArray) {
        val address = peerIp ?: return
        if (peerSupportsRepair <= 0 || indices.isEmpty()) return
        if (indices.size > peerSupportsRepair) return
        repairRequests++
        val buffer = java.nio.ByteBuffer.allocate(20 + indices.size)
            .order(java.nio.ByteOrder.LITTLE_ENDIAN)
        buffer.put(Wire.R19N.toByteArray(Charsets.US_ASCII))
        buffer.putInt(session)
        buffer.putInt(seq)
        buffer.putInt(pts.toInt())
        buffer.putInt(indices.size)
        for (index in indices) buffer.put(index.toByte())
        sendBinary(buffer.array(), address, peerPort)
    }

    // ---------------------------------------------------------------- tick

    private fun tickLoop() {
        while (running.get()) {
            try {
                Thread.sleep(100)
            } catch (_: InterruptedException) {
                return
            }
            if (!running.get()) return
            val now = SystemClock.elapsedRealtimeNanos()
            try {
                tick(now)
            } catch (_: Throwable) {
            }
        }
    }

    private fun tick(now: Long) {
        val address = peerIp
        if (address == null || now - lastPeerSeen > 4_000_000_000L) {
            if (address != null) {
                peerIp = null
                session = 0
                resolver.reset()
                assembler.reset()
                setPhase("inviting", null)
            }
            invite(now)
        } else {
            if (now - lastInviteNanos > 1_000_000_000L) {
                challenge = Random.nextInt()
                sendBinary(
                    Wire.message(Wire.DISCOVERY_PROBE, 1),
                    address,
                    8002,
                )
                lastInviteNanos = now
            }
            sendConfiguration(force = false)
            sendFeedback(now)
            sendAcks(now)
            if (now - lastTraffic > 1_500_000_000L && phase == "live") {
                setPhase("holding", "掌机暂时没有新的画面")
            }
        }
        assembler.poll(now, peerSupportsRepair, (settings.bufferMs + 90).toLong() * 1_000_000L)
        emitWindow(now)
    }

    private fun invite(now: Long) {
        if (!settings.autoDiscover && settings.handheldIp.isNotEmpty()) {
            sendBinary(Wire.message(Wire.DISCOVERY_PROBE, 1), settings.handheldIp, 8002)
            lastInviteNanos = now
            return
        }
        if (now - lastInviteNanos < 1_000_000_000L) return
        lastInviteNanos = now
        challenge = Random.nextInt()
        previousChallenge = challenge
        val probe = Wire.message(Wire.DISCOVERY_PROBE, 1)
        sendBroadcast(probe)
    }

    private fun sendBroadcast(probe: ByteArray) {
        try {
            socket?.send(DatagramPacket(probe, probe.size, InetAddress.getByName("255.255.255.255"), 8002))
        } catch (_: Throwable) {
        }
        for (network in NetworkInterface.getNetworkInterfaces()) {
            for (interfaceAddress in network.interfaceAddresses) {
                val broadcast = interfaceAddress.broadcast ?: continue
                if (broadcast.isLoopbackAddress) continue
                try {
                    socket?.send(DatagramPacket(probe, probe.size, broadcast, 8002))
                } catch (_: Throwable) {
                }
            }
        }
    }

    private fun sendConfiguration(force: Boolean) {
        val address = peerIp ?: return
        val now = SystemClock.elapsedRealtimeNanos()
        val needsCombined = !assembler.combined
        val qualityMismatch = peerSupportsQuality && appliedQuality != settings.quality
        if (!force && !needsCombined && !qualityMismatch && now - lastConfigNanos < 1_000_000_000L) return
        if (!force && now - lastConfigNanos < 500_000_000L) return
        lastConfigNanos = now
        val requestSeq = configSeq++
        sendBinary(
            Wire.message(Wire.R6CF, session, requestSeq, settings.fps, rate.requested),
            address,
            peerPort,
        )
        sendBinary(
            Wire.message(Wire.R10C, session, requestSeq, settings.fps, rate.requested, 1),
            address,
            peerPort,
        )
        if (peerSupportsQuality) {
            sendBinary(
                Wire.message(Wire.R16C, session, requestSeq, settings.fps, rate.requested, 1, settings.quality),
                address,
                peerPort,
            )
        }
    }

    private fun sendFeedback(now: Long) {
        val address = peerIp ?: return
        if (now - lastFeedbackNanos < 500_000_000L) return
        lastFeedbackNanos = now
        val elapsedUs = (now - sessionStarted) / 1000
        val request = Wire.message(
            Wire.R3FB,
            session,
            (elapsedUs / 1000).toInt(),
            packets.toInt(),
            (pairs * 2).toInt(),
            (incompletePairs * 2).toInt(),
            settings.bufferMs,
        )
        sendBinary(request, address, peerPort)
    }

    private fun sendAcks(now: Long) {
        val address = peerIp ?: return
        if (now - lastAckNanos < 1_000_000_000L) return
        lastAckNanos = now
        val ref = resolver.referenceSeq
        if (ref != null) {
            sendBinary(Wire.message(Wire.R17A, session, 1, ref), address, peerPort)
        }
    }

    private fun sendBinary(data: ByteArray, address: String, port: Int) {
        val datagramSocket = socket ?: return
        try {
            datagramSocket.send(DatagramPacket(data, data.size, InetAddress.getByName(address), port))
        } catch (_: Throwable) {
        }
    }

    // ---------------------------------------------------------------- state

    private fun setPhase(next: String, note: String?) {
        if (phase == next && message == note) return
        phase = next
        message = note
        emitState()
    }

    private fun emitState() {
        events(mapOf("type" to "state", "phase" to phase, "message" to message))
    }

    private fun emitDevices() {
        val address = peerIp ?: return
        events(
            mapOf(
                "type" to "devices",
                "devices" to listOf(
                    mapOf("ip" to address, "port" to 8002, "session" to session),
                ),
            ),
        )
    }

    private fun emit(type: String, payload: Map<String, Any?>) {
        events(mapOf("type" to type) + payload)
    }

    private fun emitWindow(now: Long) {
        val elapsed = now - windowStart
        if (elapsed < 1_000_000_000L) return
        val seconds = elapsed / 1_000_000_000.0
        val completeFps = windowPairs / seconds
        val received = windowBytes / seconds
        val sent = windowSentBytes / seconds
        val rowPercent = if (windowRowsTotal > 0) windowRowsSaved.toDouble() / windowRowsTotal else 0.0
        val lz4Percent = if (windowLz4Total > 0) windowLz4Saved.toDouble() / windowLz4Total else 0.0
        val stats = mapOf(
            "complete_fps" to completeFps,
            "top_fps" to completeFps,
            "source_fps" to completeFps,
            "quality" to (if (lastQuality > 0) lastQuality else deviceAppliedQuality),
            "cap_bytes" to cap,
            "sent_bytes_per_second" to sent,
            "received_bytes_per_second" to received,
            "dropped_pairs" to statsIncomplete.toInt(),
            "repaired_pairs" to repairedPairs.toInt(),
            "repair_timeouts" to repairTimeouts.toInt(),
            "row_savings_percent" to (rowPercent * 100.0),
            "lz4_savings_percent" to (lz4Percent * 100.0),
            "audio_seconds" to (audio.blocksPlayed * AudioPlayer.BLOCK_SAMPLES / AudioPlayer.SOURCE_RATE),
            "audio_gap_seconds" to (audio.droppedBlocks * AudioPlayer.BLOCK_SAMPLES / AudioPlayer.SOURCE_RATE),
            "buffer_ms" to settings.bufferMs.toDouble(),
            "packet_loss_percent" to 0.0,
            "window_seconds" to seconds,
        )
        if (settings.adaptive && pairs > 0 && rate.observe(completeFps, settings.fps, seconds) != null) {
            sendConfiguration(force = true)
        }
        events(mapOf("type" to "stats", "stats" to stats))
        if (seconds >= 4) {
            events(mapOf("type" to "diagnostics", "window" to stats))
        }
        windowStart = now
        windowPairs = 0
        windowPackets = 0
        windowBytes = 0
        windowSentBytes = 0
        windowRowsSaved = 0
        windowRowsTotal = 0
        windowLz4Saved = 0
        windowLz4Total = 0
        lastMediaBytes = -1
    }

    fun exportDiagnostics(): Map<String, Any?> = mapOf(
        "window_seconds" to (SystemClock.elapsedRealtimeNanos() - windowStarted) / 1_000_000_000.0,
        "pairs" to pairs.toInt(),
        "packets" to packets.toInt(),
        "incomplete_pairs" to statsIncomplete.toInt(),
        "repair_requests" to repairRequests.toInt(),
        "repair_packets" to repairPackets.toInt(),
        "quality" to lastQuality,
        "cap_bytes" to cap,
        "requested_rate" to rate.requested,
        "settings" to settings.toString(),
    )
}
