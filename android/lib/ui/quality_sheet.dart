import 'package:flutter/material.dart';

import '../stream/models.dart';
import '../theme/app_theme.dart';
import 'widgets.dart';

const List<int> _rateSteps = [
  400000, 500000, 600000, 700000, 800000, 1000000, 1200000,
  1400000, 1600000, 1800000, 2000000,
];

Future<void> showQualitySheet(BuildContext context) {
  return showModalBottomSheet<void>(
    context: context,
    isScrollControlled: true,
    useSafeArea: true,
    backgroundColor: Colors.transparent,
    builder: (_) => const QualitySheet(),
  );
}

class QualitySheet extends StatefulWidget {
  const QualitySheet({super.key});

  @override
  State<QualitySheet> createState() => _QualitySheetState();
}

class _QualitySheetState extends State<QualitySheet> {
  late StreamSettings _draft;
  bool _initialized = false;
  bool _applying = false;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (!_initialized) {
      _draft = context.live.settings;
      _initialized = true;
    }
  }

  String? _activePreset() {
    for (final preset in kStreamPresets) {
      if (_draft.fps == preset.fps &&
          _draft.jpegQuality == preset.jpegQuality &&
          _draft.rate == preset.rate &&
          !_draft.adaptiveRate) {
        return preset.id;
      }
    }
    return null;
  }

  void _applyPreset(StreamPreset preset) {
    setState(() {
      _draft = _draft.copyWith(
        fps: preset.fps,
        jpegQuality: preset.jpegQuality,
        rate: preset.rate,
        adaptiveRate: false,
      );
    });
  }

  Future<void> _apply() async {
    setState(() => _applying = true);
    await context.live.applySettings(_draft);
    if (!mounted) return;
    setState(() => _applying = false);
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final controller = context.live;
    final active = _activePreset();
    return Container(
      decoration: BoxDecoration(
        color: theme.colorScheme.surface,
        borderRadius: const BorderRadius.vertical(top: Radius.circular(kSheetRadius)),
        border: Border(top: BorderSide(color: context.hairline)),
      ),
      padding: const EdgeInsets.fromLTRB(16, 10, 16, 16),
      child: SingleChildScrollView(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Center(
              child: Container(
                width: 36,
                height: 4,
                decoration: BoxDecoration(
                  color: context.mutedColor.withValues(alpha: 0.35),
                  borderRadius: BorderRadius.circular(2),
                ),
              ),
            ),
            const SizedBox(height: 14),
            Row(
              children: [
                Text('掌机推流', style: theme.textTheme.titleLarge),
                const Spacer(),
                TextButton(
                  onPressed: () => setState(() {
                    _draft = controller.settings;
                  }),
                  child: const Text('重置'),
                ),
              ],
            ),
            const SizedBox(height: 4),
            Row(
              children: [
                for (final preset in kStreamPresets) ...[
                  Expanded(
                    child: _PresetCard(
                      preset: preset,
                      selected: active == preset.id,
                      onTap: () => _applyPreset(preset),
                    ),
                  ),
                  if (preset != kStreamPresets.last) const SizedBox(width: 8),
                ],
              ],
            ),
            const SizedBox(height: 18),
            _Label('目标帧率'),
            _Segmented<int>(
              values: kFpsOptions,
              selected: _draft.fps,
              label: (value) => '$value',
              onChanged: (value) => setState(() {
                _draft = _draft.copyWith(fps: value);
              }),
            ),
            const SizedBox(height: 16),
            _Label('JPEG 质量'),
            _Segmented<int>(
              values: kQualityOptions,
              selected: _draft.jpegQuality,
              label: qualityLabel,
              onChanged: (value) => setState(() {
                _draft = _draft.copyWith(jpegQuality: value);
              }),
            ),
            const SizedBox(height: 16),
            Row(
              children: [
                _Label('带宽上限'),
                const Spacer(),
                Text(
                  formatBytesPerSecond(_draft.rate.toDouble()),
                  style: theme.textTheme.bodySmall?.copyWith(
                    fontWeight: FontWeight.w700,
                    fontFeatures: const [FontFeature.tabularFigures()],
                  ),
                ),
              ],
            ),
            Slider(
              value: _rateSteps
                  .indexWhere((step) => step >= _draft.rate)
                  .clamp(0, _rateSteps.length - 1)
                  .toDouble(),
              min: 0,
              max: (_rateSteps.length - 1).toDouble(),
              divisions: _rateSteps.length - 1,
              label: formatBytesPerSecond(_draft.rate.toDouble()),
              onChanged: (value) => setState(() {
                _draft = _draft.copyWith(rate: _rateSteps[value.round()]);
              }),
            ),
            const SizedBox(height: 4),
            _Label('带宽模式'),
            _Segmented<bool>(
              values: const [true, false],
              selected: _draft.adaptiveRate,
              label: (value) => value ? '自适应上限' : '固定上限',
              onChanged: (value) => setState(() {
                _draft = _draft.copyWith(adaptiveRate: value);
              }),
            ),
            const SizedBox(height: 18),
            Row(
              children: [
                Text(
                  '掌机确认',
                  style: theme.textTheme.bodySmall
                      ?.copyWith(color: context.mutedColor),
                ),
                const SizedBox(width: 6),
                Expanded(
                  child: Text(
                    controller.isRunning
                        ? '${qualityLabel(controller.stats.quality)} · '
                            '${formatBytesPerSecond(controller.stats.capBytes > 0 ? controller.stats.capBytes.toDouble() : controller.settings.rate.toDouble())} · '
                            '${formatFps(controller.stats.completeFps)} fps'
                        : '开始投流后显示掌机实际采用的参数',
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: controller.isRunning
                          ? theme.colorScheme.primary
                          : context.mutedColor,
                      fontWeight: FontWeight.w700,
                    ),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            SizedBox(
              width: double.infinity,
              child: FilledButton(
                onPressed: _applying ? null : _apply,
                child: _applying
                    ? const SizedBox(
                        width: 18,
                        height: 18,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Text('应用'),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _Label extends StatelessWidget {
  const _Label(this.text);
  final String text;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Text(
        text,
        style: Theme.of(context).textTheme.labelSmall?.copyWith(
              color: context.mutedColor,
            ),
      ),
    );
  }
}

class _PresetCard extends StatelessWidget {
  const _PresetCard({
    required this.preset,
    required this.selected,
    required this.onTap,
  });

  final StreamPreset preset;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Material(
      color: selected
          ? theme.colorScheme.primary.withValues(alpha: 0.10)
          : theme.colorScheme.surfaceContainerHigh,
      borderRadius: BorderRadius.circular(kCardRadius),
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(kCardRadius),
        child: Container(
          height: 92,
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 10),
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(kCardRadius),
            border: Border.all(
              color: selected ? theme.colorScheme.primary : Colors.transparent,
              width: 1.5,
            ),
          ),
          child: Stack(
            children: [
              Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(preset.name, style: theme.textTheme.titleMedium),
                  const Spacer(),
                  for (final line in [
                    '${preset.fps} FPS',
                    'Q${preset.jpegQuality}',
                    preset.bandwidthLabel,
                  ])
                    Padding(
                      padding: const EdgeInsets.only(top: 1),
                      child: Text(
                        line,
                        maxLines: 1,
                        style: theme.textTheme.bodySmall?.copyWith(
                          fontSize: 10.5,
                          height: 1.25,
                          color: context.mutedColor,
                          fontFeatures: const [FontFeature.tabularFigures()],
                        ),
                      ),
                    ),
                ],
              ),
              if (selected)
                Positioned(
                  top: 0,
                  right: 0,
                  child: Icon(Icons.check_circle_rounded,
                      size: 16, color: theme.colorScheme.primary),
                ),
            ],
          ),
        ),
      ),
    );
  }
}

class _Segmented<T> extends StatelessWidget {
  const _Segmented({
    required this.values,
    required this.selected,
    required this.label,
    required this.onChanged,
  });

  final List<T> values;
  final T selected;
  final String Function(T) label;
  final ValueChanged<T> onChanged;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: double.infinity,
      child: SegmentedButton<T>(
        segments: [
          for (final value in values)
            ButtonSegment<T>(value: value, label: Text(label(value))),
        ],
        selected: {selected},
        showSelectedIcon: false,
        onSelectionChanged: (selection) => onChanged(selection.first),
      ),
    );
  }
}
