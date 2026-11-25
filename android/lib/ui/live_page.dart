import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../stream/live_controller.dart';
import '../stream/models.dart';
import '../theme/app_theme.dart';
import 'diagnostics_page.dart';
import 'quality_sheet.dart';
import 'widgets.dart';

class LivePage extends StatefulWidget {
  const LivePage({super.key});

  @override
  State<LivePage> createState() => _LivePageState();
}

class _LivePageState extends State<LivePage> {
  Timer? _hideTimer;
  bool _controlsVisible = true;
  bool _locked = false;
  bool _zoom = false;

  bool _systemUiApplied = false;
  LiveController? _controller;

  @override
  void initState() {
    super.initState();
    SystemChrome.setPreferredOrientations(const [
      DeviceOrientation.portraitUp,
      DeviceOrientation.landscapeLeft,
      DeviceOrientation.landscapeRight,
    ]);
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (!_systemUiApplied) {
      _systemUiApplied = true;
      final controller = context.live;
      _controller = controller;
      SystemChrome.setEnabledSystemUIMode(
        controller.settings.hideSystemUi
            ? SystemUiMode.immersiveSticky
            : SystemUiMode.edgeToEdge,
      );
      if (controller.settings.keepAwake) {
        controller.setScreenOn(true);
      }
    }
    _scheduleHide();
  }

  @override
  void dispose() {
    _hideTimer?.cancel();
    SystemChrome.setEnabledSystemUIMode(SystemUiMode.edgeToEdge);
    _controller?.setScreenOn(false);
    super.dispose();
  }

  void _scheduleHide() {
    _hideTimer?.cancel();
    if (_locked) return;
    _hideTimer = Timer(const Duration(milliseconds: 3500), () {
      if (!mounted) return;
      setState(() => _controlsVisible = false);
    });
  }

  void _toggleControls() {
    setState(() => _controlsVisible = true);
    _scheduleHide();
  }

  void _toggleLock() {
    setState(() {
      _locked = !_locked;
      _controlsVisible = true;
    });
    if (_locked) {
      _hideTimer?.cancel();
    } else {
      _scheduleHide();
    }
  }

  Future<void> _openQuality() async {
    _hideTimer?.cancel();
    await showQualitySheet(context);
    if (!mounted) return;
    _scheduleHide();
  }

  Future<void> _stop() async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('停止投流？'),
        content: Text(
          context.live.recording ? '录制会同时结束。' : '画面会断开，掌机可再次按 R + SELECT 重连。',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context, false),
            child: const Text('继续观看'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(context, true),
            child: const Text('停止'),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) return;
    await context.live.stop();
    if (mounted) Navigator.of(context).pop();
  }

  Size _canvasSize(VideoLayout layout) => switch (layout) {
        VideoLayout.stacked => const Size(400, 480),
        VideoLayout.sideBySide => const Size(720, 240),
        VideoLayout.topOnly => const Size(400, 240),
        VideoLayout.bottomOnly => const Size(320, 240),
      };

  @override
  Widget build(BuildContext context) {
    final controller = context.live;
    final layout = controller.settings.layout;
    final textureId = controller.textureId;

    final canvasSize = _canvasSize(layout);
    final content = textureId != null
        ? Texture(textureId: textureId)
        : _PlaceholderCanvas(layout: layout);
    final base = SizedBox(
      width: canvasSize.width,
      height: canvasSize.height,
      child: content,
    );
    final canvas = Container(
      color: Colors.black,
      child: _zoom
          ? ClipRect(
              child: Center(
                child: Transform.scale(scale: 1.8, child: base),
              ),
            )
          : Center(
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 24),
                child: FittedBox(fit: BoxFit.contain, child: base),
              ),
            ),
    );

    return PopScope(
      canPop: true,
      onPopInvokedWithResult: (didPop, _) {
        if (didPop) {
          SystemChrome.setEnabledSystemUIMode(SystemUiMode.edgeToEdge);
        }
      },
      child: Scaffold(
        backgroundColor: Colors.black,
        body: Stack(
          children: [
            Positioned.fill(
              child: GestureDetector(
                behavior: HitTestBehavior.opaque,
                onTap: _toggleControls,
                onDoubleTap: () => setState(() => _zoom = !_zoom),
                child: InteractiveViewer(
                  minScale: 1,
                  maxScale: 3,
                  panEnabled: _zoom,
                  child: canvas,
                ),
              ),
            ),
            Positioned(
              top: 0,
              left: 0,
              right: 0,
              child: AnimatedSlide(
                offset: _controlsVisible ? Offset.zero : const Offset(0, -1),
                duration: const Duration(milliseconds: 200),
                curve: Curves.easeOutCubic,
                child: AnimatedOpacity(
                  opacity: _controlsVisible ? 1 : 0,
                  duration: const Duration(milliseconds: 200),
                  child: _TopHud(
                    stats: controller.stats,
                    phase: controller.phase,
                    locked: _locked,
                    recording: controller.recording,
                    recordingSeconds: controller.recordingSeconds,
                    onBack: () => Navigator.of(context).pop(),
                    onDiagnostics: () => Navigator.of(context).push(
                      MaterialPageRoute(
                        builder: (_) => const DiagnosticsPage(standalone: true),
                      ),
                    ),
                    onLock: _toggleLock,
                    onCapture: () async {
                      final path = await controller.capture();
                      if (path != null && context.mounted) {
                        ScaffoldMessenger.of(context).showSnackBar(
                          SnackBar(content: Text('已保存截图：$path')),
                        );
                      }
                    },
                  ),
                ),
              ),
            ),
            if (controller.phase == StreamPhase.holding)
              const Positioned(
                top: 96,
                left: 0,
                right: 0,
                child: Center(
                  child: _FrozenBadge(),
                ),
              ),
            Positioned(
              bottom: 0,
              left: 0,
              right: 0,
              child: AnimatedSlide(
                offset: _controlsVisible ? Offset.zero : const Offset(0, 1),
                duration: const Duration(milliseconds: 200),
                curve: Curves.easeOutCubic,
                child: AnimatedOpacity(
                  opacity: _controlsVisible ? 1 : 0,
                  duration: const Duration(milliseconds: 200),
                  child: _BottomDock(
                    settings: controller.settings,
                    recording: controller.recording,
                    onQuality: _openQuality,
                    onRecord: () {
                      controller.toggleRecording().then((_) {
                        if (!context.mounted) return;
                        if (!controller.recording && controller.lastError != null) {
                          ScaffoldMessenger.of(context).showSnackBar(
                            SnackBar(content: Text(controller.lastError!)),
                          );
                        }
                      });
                    },
                    onCapture: () async {
                      final path = await controller.capture();
                      if (path != null && context.mounted) {
                        ScaffoldMessenger.of(context).showSnackBar(
                          SnackBar(content: Text('已保存截图：$path')),
                        );
                      }
                    },
                    onLayout: () {
                      final next = switch (controller.settings.layout) {
                        VideoLayout.stacked => VideoLayout.sideBySide,
                        VideoLayout.sideBySide => VideoLayout.topOnly,
                        VideoLayout.topOnly => VideoLayout.bottomOnly,
                        VideoLayout.bottomOnly => VideoLayout.stacked,
                      };
                      controller.applySettings(
                        controller.settings.copyWith(layout: next),
                      );
                    },
                    onStop: _stop,
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _FrozenBadge extends StatelessWidget {
  const _FrozenBadge();

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.6),
        borderRadius: BorderRadius.circular(99),
      ),
      child: const Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(Icons.pause_circle_outline_rounded, size: 16, color: Colors.white70),
          SizedBox(width: 6),
          Text('掌机已暂停推流', style: TextStyle(color: Colors.white70, fontSize: 12)),
        ],
      ),
    );
  }
}

class _TopHud extends StatelessWidget {
  const _TopHud({
    required this.stats,
    required this.phase,
    required this.locked,
    required this.recording,
    required this.recordingSeconds,
    required this.onBack,
    required this.onDiagnostics,
    required this.onLock,
    required this.onCapture,
  });

  final StreamStats stats;
  final StreamPhase phase;
  final bool locked;
  final bool recording;
  final int recordingSeconds;
  final VoidCallback onBack;
  final VoidCallback onDiagnostics;
  final VoidCallback onLock;
  final VoidCallback onCapture;

  @override
  Widget build(BuildContext context) {
    final quality = stats.quality > 0 ? 'Q${stats.quality}' : '自动';
    final label =
        '${formatFps(stats.completeFps)} · $quality · ${formatBytesPerSecond(stats.receivedBytesPerSecond)}';
    return Container(
      padding: const EdgeInsets.fromLTRB(4, 8, 8, 32),
      decoration: const BoxDecoration(
        gradient: LinearGradient(
          begin: Alignment.topCenter,
          end: Alignment.bottomCenter,
          colors: [Color(0xE60B0F14), Color(0x000B0F14)],
        ),
      ),
      child: SafeArea(
        bottom: false,
        child: Row(
          children: [
            IconButton(
              onPressed: onBack,
              icon: const Icon(Icons.arrow_back_ios_new_rounded,
                  color: Colors.white, size: 20),
              tooltip: '返回',
            ),
            Expanded(
              child: Align(
                alignment: Alignment.centerLeft,
                child: StatusPill(
                  text: label,
                  color: phase.color,
                  onTap: onDiagnostics,
                ),
              ),
            ),
            if (recording)
              Container(
                margin: const EdgeInsets.only(right: 4),
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                decoration: BoxDecoration(
                  color: AppColors.ledError.withValues(alpha: 0.18),
                  borderRadius: BorderRadius.circular(99),
                ),
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    const Icon(Icons.fiber_manual_record,
                        size: 10, color: Color(0xFFFFB4AB)),
                    const SizedBox(width: 5),
                    Text(
                      _clock(recordingSeconds),
                      style: const TextStyle(
                        color: Color(0xFFFFB4AB),
                        fontSize: 11,
                        fontWeight: FontWeight.w700,
                        fontFeatures: [FontFeature.tabularFigures()],
                      ),
                    ),
                  ],
                ),
              ),
            _HudButton(
              icon: locked ? Icons.lock_rounded : Icons.lock_open_rounded,
              tooltip: locked ? '解锁控件' : '锁定控件',
              onPressed: onLock,
            ),
            _HudButton(
              icon: Icons.photo_camera_outlined,
              tooltip: '截图',
              onPressed: onCapture,
            ),
            _HudButton(
              icon: Icons.more_horiz_rounded,
              tooltip: '更多',
              onPressed: onDiagnostics,
            ),
          ],
        ),
      ),
    );
  }

  static String _clock(int seconds) {
    final minutes = (seconds ~/ 60).toString().padLeft(2, '0');
    final rest = (seconds % 60).toString().padLeft(2, '0');
    return '$minutes:$rest';
  }
}

class _HudButton extends StatelessWidget {
  const _HudButton({required this.icon, required this.tooltip, required this.onPressed});

  final IconData icon;
  final String tooltip;
  final VoidCallback onPressed;

  @override
  Widget build(BuildContext context) {
    return IconButton(
      onPressed: onPressed,
      tooltip: tooltip,
      icon: Icon(icon, color: Colors.white, size: 21),
    );
  }
}

class _BottomDock extends StatelessWidget {
  const _BottomDock({
    required this.settings,
    required this.recording,
    required this.onQuality,
    required this.onRecord,
    required this.onCapture,
    required this.onLayout,
    required this.onStop,
  });

  final StreamSettings settings;
  final bool recording;
  final VoidCallback onQuality;
  final VoidCallback onRecord;
  final VoidCallback onCapture;
  final VoidCallback onLayout;
  final VoidCallback onStop;

  @override
  Widget build(BuildContext context) {
    final active = _presetId(settings);
    return Container(
      padding: const EdgeInsets.fromLTRB(12, 40, 12, 12),
      decoration: const BoxDecoration(
        gradient: LinearGradient(
          begin: Alignment.bottomCenter,
          end: Alignment.topCenter,
          colors: [Color(0xF20B0F14), Color(0x000B0F14)],
        ),
      ),
      child: SafeArea(
        top: false,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            SizedBox(
              height: 34,
              child: ListView(
                scrollDirection: Axis.horizontal,
                children: [
                  for (final preset in kStreamPresets)
                    Padding(
                      padding: const EdgeInsets.only(right: 8),
                      child: _PresetChip(
                        label: preset.name,
                        selected: active == preset.id,
                        onTap: () => onQuality(),
                      ),
                    ),
                  _PresetChip(
                    label: '自定义',
                    selected: active == null,
                    onTap: onQuality,
                  ),
                ],
              ),
            ),
            const SizedBox(height: 10),
            Row(
              children: [
                _DockButton(
                  icon: recording
                      ? Icons.stop_circle_outlined
                      : Icons.fiber_manual_record_rounded,
                  color: recording ? const Color(0xFFFF8A80) : const Color(0xFFFF8A80),
                  label: recording ? '停止录制' : '录制',
                  onTap: onRecord,
                ),
                _DockButton(
                  icon: Icons.photo_camera_outlined,
                  label: '截图',
                  onTap: onCapture,
                ),
                _DockButton(
                  icon: Icons.view_agenda_outlined,
                  label: '布局',
                  onTap: onLayout,
                ),
                const Spacer(),
                SizedBox(
                  height: 44,
                  child: FilledButton.icon(
                    onPressed: onStop,
                    style: FilledButton.styleFrom(
                      backgroundColor: AppColors.ledError,
                      foregroundColor: Colors.white,
                      padding: const EdgeInsets.symmetric(horizontal: 16),
                    ),
                    icon: const Icon(Icons.stop_rounded, size: 18),
                    label: const Text('停止'),
                  ),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }

  String? _presetId(StreamSettings settings) {
    for (final preset in kStreamPresets) {
      if (settings.fps == preset.fps &&
          settings.jpegQuality == preset.jpegQuality &&
          settings.rate == preset.rate &&
          !settings.adaptiveRate) {
        return preset.id;
      }
    }
    return null;
  }
}

class _DockButton extends StatelessWidget {
  const _DockButton({
    required this.icon,
    required this.label,
    required this.onTap,
    this.color = Colors.white,
  });

  final IconData icon;
  final String label;
  final VoidCallback onTap;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(12),
      child: SizedBox(
        width: 60,
        height: 48,
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(icon, size: 20, color: color),
            const SizedBox(height: 2),
            Text(
              label,
              style: const TextStyle(color: Colors.white70, fontSize: 10),
            ),
          ],
        ),
      ),
    );
  }
}

class _PresetChip extends StatelessWidget {
  const _PresetChip({
    required this.label,
    required this.selected,
    required this.onTap,
  });

  final String label;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: selected
          ? AppColors.darkPrimary.withValues(alpha: 0.14)
          : Colors.white.withValues(alpha: 0.10),
      borderRadius: BorderRadius.circular(99),
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(99),
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 14),
          alignment: Alignment.center,
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(99),
            border: Border.all(
              color: selected
                  ? AppColors.darkPrimary.withValues(alpha: 0.55)
                  : Colors.transparent,
            ),
          ),
          child: Text(
            label,
            style: TextStyle(
              color: selected ? AppColors.darkPrimary : Colors.white70,
              fontSize: 12,
              fontWeight: FontWeight.w700,
            ),
          ),
        ),
      ),
    );
  }
}

class _PlaceholderCanvas extends StatelessWidget {
  const _PlaceholderCanvas({required this.layout});

  final VideoLayout layout;

  @override
  Widget build(BuildContext context) {
    Widget panel(String label) => Container(
          decoration: BoxDecoration(
            color: const Color(0xFF0F1115),
            border: Border.all(color: const Color(0xFF1B1E24)),
          ),
          alignment: Alignment.center,
          child: Text(
            label,
            style: const TextStyle(color: Color(0x57FFFFFF), fontSize: 12),
          ),
        );

    final top = panel('上屏 · 400×240');
    final bottom = panel('下屏 · 320×240');
    return switch (layout) {
      VideoLayout.stacked => Column(
          children: [top, const SizedBox(height: 8), bottom],
        ),
      VideoLayout.sideBySide => Row(
          children: [top, const SizedBox(width: 8), bottom],
        ),
      VideoLayout.topOnly => top,
      VideoLayout.bottomOnly => bottom,
    };
  }
}
