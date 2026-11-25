import 'dart:async';
import 'dart:convert';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'models.dart';
import 'stream_bridge.dart';

/// Application state: one controller owns the engine session and everything the
/// UI renders. Kotlin pushes events; Dart never parses protocol packets.
class LiveController extends ChangeNotifier {
  LiveController({StreamBridge? bridge, bool? mock})
      : _bridge = bridge ?? StreamBridge(),
        _forceMock = mock ?? false;

  final StreamBridge _bridge;
  final bool _forceMock;

  StreamSubscription<Map<String, dynamic>>? _events;
  Timer? _mockTicker;
  Timer? _recordTicker;
  final math.Random _random = math.Random(7);

  StreamPhase phase = StreamPhase.idle;
  String? message;
  String? lastError;
  bool busy = false;
  bool mockMode = false;

  List<Handheld> devices = const [];
  Handheld? recent;
  StreamStats stats = const StreamStats();
  List<StreamStats> history = const [];
  int? textureId;

  bool recording = false;
  String? recordingPath;
  int recordingSeconds = 0;
  String? lastCapturePath;

  StreamSettings settings = const StreamSettings();

  bool get isRunning => phase.isRunning;
  bool get isLive => phase == StreamPhase.live;

  Future<void> initialize() async {
    if (_forceMock) {
      _enterMock('已按启动参数进入演示模式');
      notifyListeners();
      return;
    }
    try {
      _events ??= _bridge.events.listen(_onEvent, onError: (Object error) {
        lastError = '$error';
        notifyListeners();
      });
      textureId = await _bridge.textureId();
      final stored = await _bridge.loadSettings();
      if (stored != null && stored.isNotEmpty) {
        settings = StreamSettings.fromJson(
          jsonDecode(stored) as Map<String, dynamic>,
        );
      }
    } on MissingPluginException {
      _enterMock('未检测到原生接收引擎，已切换到演示模式');
    } on PlatformException catch (error) {
      _enterMock('原生接收引擎不可用：${error.message ?? error.code}');
    }
    notifyListeners();
  }

  Future<void> start() async {
    if (isRunning) return;
    busy = true;
    message = null;
    lastError = null;
    phase = StreamPhase.inviting;
    notifyListeners();
    if (mockMode) {
      _startMockSession();
      busy = false;
      return;
    }
    try {
      await _bridge.start(settings.toJson());
    } on MissingPluginException {
      _enterMock('未检测到原生接收引擎，已切换到演示模式');
      _startMockSession();
    } on PlatformException catch (error) {
      phase = StreamPhase.error;
      lastError = error.message ?? error.code;
      message = '无法开始接收：${error.message ?? error.code}';
    } finally {
      busy = false;
      notifyListeners();
    }
  }

  Future<void> stop() async {
    _stopMockSession();
    stopRecording();
    if (!mockMode) {
      try {
        await _bridge.stop();
      } on PlatformException catch (error) {
        lastError = error.message ?? error.code;
      }
    }
    phase = StreamPhase.idle;
    message = null;
    unawaited(setScreenOn(false));
    stats = const StreamStats();
    history = const [];
    notifyListeners();
  }

  Future<void> connectTo(Handheld handheld) async {
    recent = handheld;
    settings = settings.copyWith(handheldIp: handheld.ip);
    await start();
  }

  Future<void> applySettings(StreamSettings next) async {
    final previous = settings;
    settings = next;
    notifyListeners();
    unawaited(_persistSettings());
    if (!isRunning || mockMode) return;
    try {
      await _bridge.configure(next.toJson());
      if (previous.layout != next.layout) {
        await _bridge.setLayout(next.layout.name);
      }
    } on PlatformException catch (error) {
      lastError = error.message ?? error.code;
      notifyListeners();
    }
  }

  Future<void> _persistSettings() async {
    if (mockMode) return;
    try {
      await _bridge.saveSettings(jsonEncode(settings.toJson()));
    } on PlatformException catch (_) {
      // Persistence is best-effort; the engine keeps the live settings.
    }
  }

  /// Keeps the display awake while the live view is in front.
  Future<void> setScreenOn(bool on) async {
    if (mockMode) return;
    try {
      await _bridge.setKeepScreenOn(on);
    } on PlatformException catch (_) {
    }
  }

  Future<void> viewAttached() async {
    if (mockMode) return;
    try {
      await _bridge.viewAttached();
    } on PlatformException catch (_) {
    }
  }

  Future<String?> capture() async {
    if (mockMode) {
      lastCapturePath = '/sdcard/Pictures/3DS Live/capture-demo.png';
      return lastCapturePath;
    }
    try {
      lastCapturePath = await _bridge.capture();
      return lastCapturePath;
    } on PlatformException catch (error) {
      lastError = error.message ?? error.code;
      return null;
    }
  }

  Future<void> toggleRecording() async {
    if (recording) {
      stopRecording();
      return;
    }
    if (mockMode) {
      recording = true;
      recordingSeconds = 0;
      recordingPath = '/sdcard/Movies/3DS Live/demo.mp4';
      _recordTicker = Timer.periodic(const Duration(seconds: 1), (_) {
        recordingSeconds++;
        notifyListeners();
      });
      notifyListeners();
      return;
    }
    try {
      recordingPath = await _bridge.startRecording();
      recording = recordingPath != null;
      if (!recording) {
        lastError = '录制启动失败：设备可能不支持 H.264 编码，或存储不可写。';
      }
      recordingSeconds = 0;
      if (recording) {
        _recordTicker = Timer.periodic(const Duration(seconds: 1), (_) {
          recordingSeconds++;
          notifyListeners();
        });
      }
    } on PlatformException catch (error) {
      lastError = error.message ?? error.code;
    }
    notifyListeners();
  }

  void stopRecording() {
    _recordTicker?.cancel();
    _recordTicker = null;
    if (!recording) return;
    recording = false;
    if (!mockMode) {
      _bridge.stopRecording().catchError((Object _) {});
    }
    notifyListeners();
  }

  Future<Map<String, dynamic>?> exportDiagnostics() async {
    if (mockMode) {
      return {
        'window_seconds': 4.0,
        'complete_fps': stats.completeFps,
        'received_bytes_per_second': stats.receivedBytesPerSecond,
        'quality': stats.quality,
      };
    }
    try {
      return await _bridge.exportDiagnostics();
    } on PlatformException catch (error) {
      lastError = error.message ?? error.code;
      return null;
    }
  }

  void clearRecent() {
    recent = null;
    notifyListeners();
  }

  void _onEvent(Map<String, dynamic> event) {
    switch (event['type']) {
      case 'state':
        final name = event['phase'] as String?;
        phase = StreamPhase.values.firstWhere(
          (value) => value.name == name,
          orElse: () => phase,
        );
        message = event['message'] as String?;
      case 'devices':
        final list = (event['devices'] as List?) ?? const [];
        devices = list
            .whereType<Map>()
            .map((item) => Handheld.fromJson(Map<String, dynamic>.from(item)))
            .toList();
      case 'stats':
        final next = StreamStats.fromJson(
          Map<String, dynamic>.from((event['stats'] as Map?) ?? const {}),
        );
        if (next.windowSeconds > 0 || next.completeFps > 0 || next.receivedBytesPerSecond > 0) {
          _appendStats(next);
        }
      case 'diagnostics':
        final next = StreamStats.fromJson(
          Map<String, dynamic>.from((event['window'] as Map?) ?? const {}),
        );
        if (next.windowSeconds > 0) _appendStats(next, replaceLatest: true);
      case 'recording':
        recording = event['active'] as bool? ?? false;
        recordingPath = event['path'] as String?;
        if (!recording) stopRecording();
      case 'error':
        lastError = event['message'] as String? ?? '未知错误';
        phase = StreamPhase.error;
    }
    notifyListeners();
  }

  void _appendStats(StreamStats next, {bool replaceLatest = false}) {
    final list = [...history];
    if (replaceLatest && list.isNotEmpty) {
      list[list.length - 1] = next;
    } else {
      list.add(next);
      while (list.length > 60) {
        list.removeAt(0);
      }
    }
    stats = next;
    history = list;
  }

  void _enterMock(String reason) {
    mockMode = true;
    message = reason;
  }

  void _startMockSession() {
    phase = StreamPhase.inviting;
    devices = const [
      Handheld(ip: '192.168.1.23', session: 0x8F2A),
      Handheld(ip: '192.168.1.31', session: 0x3C19),
    ];
    recent ??= const Handheld(ip: '192.168.1.23', session: 0x8F2A);
    _mockTicker?.cancel();
    var tick = 0;
    _mockTicker = Timer.periodic(const Duration(seconds: 1), (timer) {
      tick++;
      if (tick == 2) {
        phase = StreamPhase.live;
        message = null;
      }
      if (tick >= 2) {
        final fps = 30 - (tick % 11 == 7 ? 6 : 0) - _random.nextDouble() * 0.4;
        final next = StreamStats(
          completeFps: fps,
          topFps: fps - 0.1,
          sourceFps: fps,
          quality: settings.jpegQuality == 0 ? 70 : settings.jpegQuality,
          capBytes: settings.rate,
          sentBytesPerSecond: 1_920_000,
          receivedBytesPerSecond: 1_780_000,
          droppedPairs: 12,
          repairedPairs: 9,
          repairTimeouts: 3,
          rowSavingsPercent: 38,
          lz4SavingsPercent: 6,
          audioSeconds: 60,
          audioGapSeconds: 0.4,
          bufferMs: settings.bufferMs.toDouble(),
          packetLossPercent: 1.8,
          windowSeconds: 4,
          events: tick % 9 == 0
              ? const ['自适应降档 2.00 → 1.80 MB/s']
              : (tick % 13 == 0
                  ? const ['参考帧重建 · 行复用恢复']
                  : const []),
        );
        _appendStats(next);
      }
      notifyListeners();
    });
    notifyListeners();
  }

  void _stopMockSession() {
    _mockTicker?.cancel();
    _mockTicker = null;
    devices = const [];
  }

  @override
  void dispose() {
    _events?.cancel();
    _mockTicker?.cancel();
    _recordTicker?.cancel();
    super.dispose();
  }
}
