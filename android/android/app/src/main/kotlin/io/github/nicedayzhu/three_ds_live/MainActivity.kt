package io.github.nicedayzhu.three_ds_live

import android.content.Intent
import android.os.Build
import android.os.Handler
import android.util.Log
import android.os.Looper
import android.view.WindowManager
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.github.nicedayzhu.three_ds_live.stream.EngineHolder
import io.github.nicedayzhu.three_ds_live.stream.Logs
import io.github.nicedayzhu.three_ds_live.stream.StreamEngine

class MainActivity : FlutterActivity() {
    private var engine: StreamEngine? = null
    private var eventSink: EventChannel.EventSink? = null
    private val main = Handler(Looper.getMainLooper())
    private val pending = ArrayDeque<Map<String, Any?>>()

    override fun onCreate(savedInstanceState: android.os.Bundle?) {
        super.onCreate(savedInstanceState)
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            Log.e("3ds-live", "uncaught on ${thread.name}", error)
            try {
                java.io.File(filesDir, "crash-last.txt").writeText(
                    "thread=${thread.name}\n" + Log.getStackTraceString(error),
                )
            } catch (_: Throwable) {
            }
            previous?.uncaughtException(thread, error)
        }
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        EventChannel(flutterEngine.dartExecutor.binaryMessenger, "3ds_live/events")
            .setStreamHandler(object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink) {
                    eventSink = events
                    for (event in pending) events.success(event)
                    pending.clear()
                }

                override fun onCancel(arguments: Any?) {
                    eventSink = null
                }
            })

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "3ds_live/stream")
            .setMethodCallHandler { call, result -> handle(call, result) }
    }

    private fun streamEngine(): StreamEngine {
        val existing = engine
        if (existing != null) return existing
        val flutterEngine = flutterEngine ?: error("FlutterEngine is not ready")
        val created = StreamEngine(applicationContext, flutterEngine.renderer, ::emit)
        engine = created
        EngineHolder.engine = created
        return created
    }

    private fun emit(event: Map<String, Any?>) {
        main.post {
            val sink = eventSink
            if (sink != null) {
                sink.success(event)
            } else {
                if (pending.size > 32) pending.removeFirst()
                pending.addLast(event)
            }
        }
    }

    private fun handle(call: MethodCall, result: MethodChannel.Result) {
        try {
            when (call.method) {
                "textureId" -> result.success(streamEngine().refreshTexture())
                "start" -> {
                    val settings = StreamEngine.Settings.fromMap(call.arguments as? Map<*, *>)
                    startService()
                    streamEngine().start(settings)
                    result.success(null)
                }
                "stop" -> {
                    engine?.stop()
                    stopService()
                    result.success(null)
                }
                "configure" -> {
                    engine?.applySettings(
                        StreamEngine.Settings.fromMap(call.arguments as? Map<*, *>),
                    )
                    result.success(null)
                }
                "setLayout" -> {
                    val layout = call.arguments as? String
                    if (layout != null) engine?.setLayout(layout)
                    result.success(null)
                }
                "capture" -> result.success(engine?.capture())
                "viewAttached" -> {
                    engine?.onViewAttached()
                    result.success(null)
                }
                "exportLog" -> {
                    try {
                        result.success(Logs.export(applicationContext))
                    } catch (error: Throwable) {
                        Logs.e("3ds-live", "export log failed", error)
                        result.error("export", error.message, null)
                    }
                }
                "setKeepScreenOn" -> {
                    val on = call.arguments as? Boolean ?: false
                    runOnUiThread {
                        if (on) {
                            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                        } else {
                            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                        }
                    }
                    result.success(null)
                }
                "loadSettings" -> result.success(
                    getSharedPreferences("3ds_live", MODE_PRIVATE)
                        .getString("settings", null),
                )
                "saveSettings" -> {
                    val json = call.arguments as? String ?: ""
                    getSharedPreferences("3ds_live", MODE_PRIVATE)
                        .edit().putString("settings", json).apply()
                    result.success(null)
                }
                "startRecording" -> result.success(engine?.startRecording())
                "stopRecording" -> { engine?.stopRecording(); result.success(null) }
                "exportDiagnostics" ->
                    result.success(engine?.exportDiagnostics() ?: emptyMap<String, Any?>())
                else -> result.notImplemented()
            }
        } catch (error: Throwable) {
            result.error("engine", error.message ?: error.javaClass.simpleName, null)
        }
    }

    private fun startService() {
        val intent = Intent(this, StreamService::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }
    }

    private fun stopService() {
        stopService(Intent(this, StreamService::class.java))
    }

    override fun onDestroy() {
        engine?.release()
        engine = null
        EngineHolder.engine = null
        super.onDestroy()
    }
}
