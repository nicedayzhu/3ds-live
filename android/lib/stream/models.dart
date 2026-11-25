import 'package:flutter/material.dart';

import '../theme/app_theme.dart';

/// Lifecycle of the receive pipeline, mirroring the Qt player's discovery
/// state machine (docs/android/DESIGN.md §3.1).
enum StreamPhase {
  idle('空闲', AppColors.ledIdle),
  inviting('正在寻找掌机', AppColors.ledSearching),
  reserved('掌机已应答', AppColors.ledSearching),
  live('正在投流', AppColors.ledStreaming),
  holding('已停顿', AppColors.ledSearching),
  lost('连接已断开', AppColors.ledSearching),
  error('连接失败', AppColors.ledError);

  const StreamPhase(this.label, this.color);
  final String label;
  final Color color;

  bool get isRunning => this == live || this == reserved || this == holding;
}

enum VideoLayout {
  stacked('上下双屏'),
  sideBySide('并排双屏'),
  topOnly('仅上屏'),
  bottomOnly('仅下屏');

  const VideoLayout(this.label);
  final String label;
}

class Handheld {
  const Handheld({
    required this.ip,
    this.port = 8002,
    this.session = 0,
    this.lastSeen,
    this.rssiHint,
  });

  final String ip;
  final int port;
  final int session;
  final DateTime? lastSeen;
  final int? rssiHint;

  String get displayName => 'New 3DS · $ip';

  Map<String, dynamic> toJson() => {
        'ip': ip,
        'port': port,
        'session': session,
        'lastSeen': lastSeen?.millisecondsSinceEpoch,
      };

  static Handheld fromJson(Map<String, dynamic> json) => Handheld(
        ip: json['ip'] as String? ?? '',
        port: (json['port'] as num?)?.toInt() ?? 8002,
        session: (json['session'] as num?)?.toInt() ?? 0,
        lastSeen: json['lastSeen'] is num
            ? DateTime.fromMillisecondsSinceEpoch(
                (json['lastSeen'] as num).toInt())
            : null,
      );
}

/// One second of aligned diagnostics, as produced by the Kotlin telemetry
/// window (the Android equivalent of the Qt player's F12 panel).
class StreamStats {
  const StreamStats({
    this.completeFps = 0,
    this.topFps = 0,
    this.sourceFps = 0,
    this.quality = 0,
    this.capBytes = 0,
    this.sentBytesPerSecond = 0,
    this.receivedBytesPerSecond = 0,
    this.droppedPairs = 0,
    this.repairedPairs = 0,
    this.repairTimeouts = 0,
    this.rowSavingsPercent = 0,
    this.lz4SavingsPercent = 0,
    this.audioSeconds = 0,
    this.audioGapSeconds = 0,
    this.bufferMs = 100,
    this.packetLossPercent = 0,
    this.windowSeconds = 0,
    this.events = const [],
  });

  final double completeFps;
  final double topFps;
  final double sourceFps;
  final int quality;
  final int capBytes;
  final double sentBytesPerSecond;
  final double receivedBytesPerSecond;
  final int droppedPairs;
  final int repairedPairs;
  final int repairTimeouts;
  final double rowSavingsPercent;
  final double lz4SavingsPercent;
  final double audioSeconds;
  final double audioGapSeconds;
  final double bufferMs;
  final double packetLossPercent;
  final double windowSeconds;
  final List<String> events;

  bool get hasData => completeFps > 0 || receivedBytesPerSecond > 0;

  static StreamStats fromJson(Map<String, dynamic> json) => StreamStats(
        completeFps: _d(json['complete_fps']),
        topFps: _d(json['top_fps']),
        sourceFps: _d(json['source_fps']),
        quality: (json['quality'] as num?)?.toInt() ?? 0,
        capBytes: (json['cap_bytes'] as num?)?.toInt() ?? 0,
        sentBytesPerSecond: _d(json['sent_bytes_per_second']),
        receivedBytesPerSecond: _d(json['received_bytes_per_second']),
        droppedPairs: (json['dropped_pairs'] as num?)?.toInt() ?? 0,
        repairedPairs: (json['repaired_pairs'] as num?)?.toInt() ?? 0,
        repairTimeouts: (json['repair_timeouts'] as num?)?.toInt() ?? 0,
        rowSavingsPercent: _d(json['row_savings_percent']),
        lz4SavingsPercent: _d(json['lz4_savings_percent']),
        audioSeconds: _d(json['audio_seconds']),
        audioGapSeconds: _d(json['audio_gap_seconds']),
        bufferMs: _d(json['buffer_ms']),
        packetLossPercent: _d(json['packet_loss_percent']),
        windowSeconds: _d(json['window_seconds']),
        events: (json['events'] as List?)?.map((e) => '$e').toList() ??
            const [],
      );

  static double _d(Object? value) =>
      value is num ? value.toDouble() : 0;
}

class StreamPreset {
  const StreamPreset(this.id, this.name, this.fps, this.jpegQuality, this.rate);

  final String id;
  final String name;
  final int fps;
  final int jpegQuality;
  final int rate;

  String get bandwidthLabel => '${(rate / 1000000).toStringAsFixed(2)} MB/s';
}

/// Same three presets as the Qt player (live/qt/stream_presets.h).
const List<StreamPreset> kStreamPresets = [
  StreamPreset('hd30', '高清', 30, 80, 2000000),
  StreamPreset('balanced30', '均衡', 30, 70, 2000000),
  StreamPreset('low24', '低带宽', 24, 40, 1000000),
  StreamPreset('saver30', '省流', 30, 20, 800000),
];

const List<int> kFpsOptions = [15, 20, 24, 30];
const List<int> kQualityOptions = [0, 20, 40, 60, 80];

String qualityLabel(int quality) =>
    quality == 0 ? '自动' : 'Q$quality';

String formatBytesPerSecond(double bytes) {
  if (bytes <= 0) return '—';
  if (bytes >= 1000000) return '${(bytes / 1000000).toStringAsFixed(2)} MB/s';
  return '${(bytes / 1000).toStringAsFixed(0)} kB/s';
}

String formatFps(double fps) =>
    fps <= 0 ? '—' : fps.toStringAsFixed(1);

class StreamSettings {
  const StreamSettings({
    this.port = 8001,
    this.handheldIp = '',
    this.autoDiscover = true,
    this.fps = 30,
    this.rate = 800000,
    this.jpegQuality = 0,
    this.adaptiveRate = true,
    this.audioPriority = true,
    this.bufferMs = 100,
    this.layout = VideoLayout.stacked,
    this.keepAwake = true,
    this.hideSystemUi = true,
    this.volume = 1.0,
    this.muted = false,
    this.recordResolution = '720p',
    this.recordBitrate = 8000000,
  });

  final int port;
  final String handheldIp;
  final bool autoDiscover;
  final int fps;
  final int rate;
  final int jpegQuality;
  final bool adaptiveRate;
  final bool audioPriority;
  final int bufferMs;
  final VideoLayout layout;
  final bool keepAwake;
  final bool hideSystemUi;
  final double volume;
  final bool muted;
  final String recordResolution;
  final int recordBitrate;

  StreamSettings copyWith({
    int? port,
    String? handheldIp,
    bool? autoDiscover,
    int? fps,
    int? rate,
    int? jpegQuality,
    bool? adaptiveRate,
    bool? audioPriority,
    int? bufferMs,
    VideoLayout? layout,
    bool? keepAwake,
    bool? hideSystemUi,
    double? volume,
    bool? muted,
    String? recordResolution,
    int? recordBitrate,
  }) {
    return StreamSettings(
      port: port ?? this.port,
      handheldIp: handheldIp ?? this.handheldIp,
      autoDiscover: autoDiscover ?? this.autoDiscover,
      fps: fps ?? this.fps,
      rate: rate ?? this.rate,
      jpegQuality: jpegQuality ?? this.jpegQuality,
      adaptiveRate: adaptiveRate ?? this.adaptiveRate,
      audioPriority: audioPriority ?? this.audioPriority,
      bufferMs: bufferMs ?? this.bufferMs,
      layout: layout ?? this.layout,
      keepAwake: keepAwake ?? this.keepAwake,
      hideSystemUi: hideSystemUi ?? this.hideSystemUi,
      volume: volume ?? this.volume,
      muted: muted ?? this.muted,
      recordResolution: recordResolution ?? this.recordResolution,
      recordBitrate: recordBitrate ?? this.recordBitrate,
    );
  }

  Map<String, dynamic> toJson() => {
        'port': port,
        'handheld_ip': handheldIp,
        'auto_discover': autoDiscover,
        'fps': fps,
        'rate': rate,
        'jpeg_quality': jpegQuality,
        'adaptive_rate': adaptiveRate,
        'buffer_ms': bufferMs,
        'layout': layout.name,
        'keep_awake': keepAwake,
        'hide_system_ui': hideSystemUi,
        'volume': volume,
        'muted': muted,
        'record_resolution': recordResolution,
        'record_bitrate': recordBitrate,
      };

  static StreamSettings fromJson(Map<String, dynamic> json) => StreamSettings(
        port: (json['port'] as num?)?.toInt() ?? 8001,
        handheldIp: json['handheld_ip'] as String? ?? '',
        autoDiscover: json['auto_discover'] as bool? ?? true,
        fps: (json['fps'] as num?)?.toInt() ?? 30,
        rate: (json['rate'] as num?)?.toInt() ?? 800000,
        jpegQuality: (json['jpeg_quality'] as num?)?.toInt() ?? 0,
        adaptiveRate: json['adaptive_rate'] as bool? ?? true,
        bufferMs: (json['buffer_ms'] as num?)?.toInt() ?? 100,
        layout: VideoLayout.values.firstWhere(
          (l) => l.name == json['layout'],
          orElse: () => VideoLayout.stacked,
        ),
        keepAwake: json['keep_awake'] as bool? ?? true,
        hideSystemUi: json['hide_system_ui'] as bool? ?? true,
        volume: (json['volume'] as num?)?.toDouble() ?? 1,
        muted: json['muted'] as bool? ?? false,
        recordResolution: json['record_resolution'] as String? ?? '720p',
        recordBitrate: (json['record_bitrate'] as num?)?.toInt() ?? 8000000,
      );
}
