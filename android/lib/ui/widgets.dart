import 'package:flutter/material.dart';

import '../stream/live_controller.dart';
import '../stream/models.dart';
import '../theme/app_theme.dart';

/// Shared widgets from docs/android/UI-SPEC.md §5.

class LiveScope extends InheritedNotifier<LiveController> {
  const LiveScope({
    super.key,
    required LiveController controller,
    required super.child,
  }) : super(notifier: controller);
}

extension LiveScopeX on BuildContext {
  LiveController get live => dependOnInheritedWidgetOfExactType<LiveScope>()!.notifier!;
}

class BrandBar extends StatelessWidget {
  const BrandBar({
    super.key,
    this.title = '3DS Live',
    this.subtitle,
    this.onTheme,
    this.onSettings,
    this.trailing,
  });

  final String title;
  final String? subtitle;
  final VoidCallback? onTheme;
  final VoidCallback? onSettings;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return SizedBox(
      height: 56,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 16),
        child: Row(
          children: [
            Container(
              width: 28,
              height: 28,
              decoration: BoxDecoration(
                color: AppColors.secondary,
                borderRadius: BorderRadius.circular(8),
              ),
              alignment: Alignment.center,
              child: Text(
                '3DS',
                style: theme.textTheme.labelSmall?.copyWith(
                  color: Colors.white,
                  fontSize: 8,
                  fontWeight: FontWeight.w800,
                ),
              ),
            ),
            const SizedBox(width: 10),
            Column(
              mainAxisAlignment: MainAxisAlignment.center,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  title,
                  style: theme.textTheme.titleLarge?.copyWith(fontSize: 17),
                ),
                if (subtitle != null)
                  Text(
                    subtitle!,
                    style: theme.textTheme.bodySmall?.copyWith(
                      fontSize: 11,
                      color: context.mutedColor,
                      height: 1.2,
                    ),
                  ),
              ],
            ),
            const Spacer(),
            ?trailing,
            if (onTheme != null)
              IconButton(
                tooltip: '切换深浅色',
                onPressed: onTheme,
                icon: Icon(
                  theme.brightness == Brightness.dark
                      ? Icons.light_mode_outlined
                      : Icons.dark_mode_outlined,
                ),
              ),
            if (onSettings != null)
              IconButton(
                tooltip: '设置',
                onPressed: onSettings,
                icon: const Icon(Icons.tune_rounded),
              ),
          ],
        ),
      ),
    );
  }
}

class SectionHeader extends StatelessWidget {
  const SectionHeader({super.key, required this.title, this.trailing});

  final String title;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(0, 24, 0, 10),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.baseline,
        textBaseline: TextBaseline.alphabetic,
        children: [
          Text(title, style: Theme.of(context).textTheme.titleLarge),
          const Spacer(),
          ?trailing,
        ],
      ),
    );
  }
}

class StatusDot extends StatelessWidget {
  const StatusDot({super.key, required this.color, this.size = 10});

  final Color color;
  final double size;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(
        color: color,
        shape: BoxShape.circle,
        boxShadow: [
          BoxShadow(color: color.withValues(alpha: 0.2), spreadRadius: 4),
        ],
      ),
    );
  }
}

class StatusCard extends StatelessWidget {
  const StatusCard({
    super.key,
    required this.phase,
    this.message,
    required this.primaryLabel,
    this.onPrimary,
    this.secondaryLabel,
    this.onSecondary,
    this.busy = false,
  });

  final StreamPhase phase;
  final String? message;
  final String primaryLabel;
  final VoidCallback? onPrimary;
  final String? secondaryLabel;
  final VoidCallback? onSecondary;
  final bool busy;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: theme.colorScheme.surfaceContainerLow,
        borderRadius: BorderRadius.circular(kCardRadius),
        border: Border.all(color: context.hairline),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              StatusDot(color: phase.color),
              const SizedBox(width: 10),
              Expanded(
                child: Text(phase.label, style: theme.textTheme.titleLarge),
              ),
            ],
          ),
          const SizedBox(height: 10),
          Text(
            message ?? _defaultHint(phase),
            style: theme.textTheme.bodyMedium?.copyWith(
              color: context.mutedColor,
            ),
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              Expanded(
                flex: 6,
                child: FilledButton(
                  onPressed: busy ? null : onPrimary,
                  child: busy
                      ? const SizedBox(
                          width: 18,
                          height: 18,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        )
                      : Text(primaryLabel),
                ),
              ),
              if (secondaryLabel != null) ...[
                const SizedBox(width: 10),
                Expanded(
                  flex: 5,
                  child: OutlinedButton(
                    onPressed: onSecondary,
                    child: Text(secondaryLabel!),
                  ),
                ),
              ],
            ],
          ),
        ],
      ),
    );
  }

  String _defaultHint(StreamPhase phase) => switch (phase) {
        StreamPhase.idle => '在掌机打开 3DS Live 后按 R + SELECT，两端会自动配对。',
        StreamPhase.inviting => '等待掌机回应，请确认两端在同一 Wi-Fi。',
        StreamPhase.reserved => '掌机已确认，正在等待第一帧画面。',
        StreamPhase.live => '画面正在传输。',
        StreamPhase.holding => '掌机暂停了推流，最后一帧会保留。',
        StreamPhase.lost => '连接已断开，可以重试或手动填写掌机 IPv4。',
        StreamPhase.error => '请检查端口、防火墙与网络后重试。',
      };
}

class DeviceCard extends StatelessWidget {
  const DeviceCard({
    super.key,
    required this.handheld,
    required this.onTap,
    this.subtitle,
  });

  final Handheld handheld;
  final VoidCallback onTap;
  final String? subtitle;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Padding(
      padding: const EdgeInsets.only(bottom: 10),
      child: Material(
        color: theme.colorScheme.surfaceContainerLow,
        borderRadius: BorderRadius.circular(kCardRadius),
        child: InkWell(
          borderRadius: BorderRadius.circular(kCardRadius),
          onTap: onTap,
          child: Container(
            padding: const EdgeInsets.all(16),
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(kCardRadius),
              border: Border.all(color: context.hairline),
            ),
            child: Row(
              children: [
                Container(
                  width: 40,
                  height: 40,
                  decoration: BoxDecoration(
                    color: theme.colorScheme.primary.withValues(alpha: 0.10),
                    borderRadius: BorderRadius.circular(12),
                  ),
                  alignment: Alignment.center,
                  child: Text(
                    '3DS',
                    style: theme.textTheme.labelSmall?.copyWith(
                      color: theme.colorScheme.primary,
                      fontWeight: FontWeight.w800,
                      fontSize: 9,
                    ),
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(handheld.displayName,
                          style: theme.textTheme.titleMedium),
                      const SizedBox(height: 3),
                      Text(
                        subtitle ?? _defaultSubtitle(handheld),
                        style: theme.textTheme.bodySmall
                            ?.copyWith(color: context.mutedColor),
                      ),
                    ],
                  ),
                ),
                Icon(Icons.chevron_right_rounded, color: context.mutedColor),
              ],
            ),
          ),
        ),
      ),
    );
  }

  String _defaultSubtitle(Handheld handheld) {
    final seen = handheld.lastSeen;
    if (seen == null) return 'session ${handheld.session.toRadixString(16).toUpperCase()}';
    final delta = DateTime.now().difference(seen);
    final when = delta.inSeconds < 60
        ? '刚刚'
        : delta.inMinutes < 60
            ? '${delta.inMinutes} 分钟前'
            : '${delta.inHours} 小时前';
    return '$when · session ${handheld.session.toRadixString(16).toUpperCase()}';
  }
}

class NoticeBanner extends StatelessWidget {
  const NoticeBanner({
    super.key,
    required this.text,
    this.icon = Icons.wifi_rounded,
    this.tone = AppColors.primary,
  });

  final String text;
  final IconData icon;
  final Color tone;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: tone.withValues(alpha: 0.06),
        borderRadius: BorderRadius.circular(kCardRadius),
        border: Border(left: BorderSide(color: tone, width: 2)),
      ),
      child: Row(
        children: [
          Icon(icon, size: 18, color: tone),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              text,
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ),
        ],
      ),
    );
  }
}

class EmptyState extends StatelessWidget {
  const EmptyState({
    super.key,
    required this.title,
    required this.body,
    this.actionLabel,
    this.onAction,
    this.icon = Icons.radar_rounded,
  });

  final String title;
  final String body;
  final String? actionLabel;
  final VoidCallback? onAction;
  final IconData icon;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 40),
      child: Column(
        children: [
          Icon(icon, size: 48, color: context.mutedColor),
          const SizedBox(height: 12),
          Text(title, style: theme.textTheme.titleMedium),
          const SizedBox(height: 6),
          Text(
            body,
            textAlign: TextAlign.center,
            style: theme.textTheme.bodySmall?.copyWith(color: context.mutedColor),
          ),
          if (actionLabel != null) ...[
            const SizedBox(height: 8),
            TextButton(onPressed: onAction, child: Text(actionLabel!)),
          ],
        ],
      ),
    );
  }
}

class MetricTile extends StatelessWidget {
  const MetricTile({
    super.key,
    required this.label,
    required this.value,
    this.unit,
    this.onTap,
  });

  final String label;
  final String value;
  final String? unit;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Material(
      color: theme.colorScheme.surfaceContainerLow,
      borderRadius: BorderRadius.circular(kCardRadius),
      child: InkWell(
        borderRadius: BorderRadius.circular(kCardRadius),
        onTap: onTap,
        child: Container(
          height: 84,
          padding: const EdgeInsets.all(12),
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(kCardRadius),
            border: Border.all(color: context.hairline),
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                label,
                style: theme.textTheme.labelSmall?.copyWith(
                  color: context.mutedColor,
                ),
              ),
              const Spacer(),
              Row(
                crossAxisAlignment: CrossAxisAlignment.baseline,
                textBaseline: TextBaseline.alphabetic,
                children: [
                  Flexible(
                    child: Text(
                      value,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: theme.textTheme.headlineSmall?.copyWith(
                        fontSize: 20,
                        height: 26 / 20,
                        fontFeatures: const [FontFeature.tabularFigures()],
                      ),
                    ),
                  ),
                  if (unit != null)
                    Text(
                      ' $unit',
                      style: theme.textTheme.labelSmall
                          ?.copyWith(color: context.mutedColor),
                    ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class StatusPill extends StatelessWidget {
  const StatusPill({super.key, required this.text, this.color = AppColors.ledStreaming, this.onTap});

  final String text;
  final Color color;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.white.withValues(alpha: 0.14),
      borderRadius: BorderRadius.circular(99),
      child: InkWell(
        borderRadius: BorderRadius.circular(99),
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              Container(
                width: 6,
                height: 6,
                decoration: BoxDecoration(color: color, shape: BoxShape.circle),
              ),
              const SizedBox(width: 7),
              Flexible(
                child: Text(
                  text,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 12,
                    fontWeight: FontWeight.w600,
                    fontFeatures: [FontFeature.tabularFigures()],
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class Sparkline extends StatelessWidget {
  const Sparkline({super.key, required this.values, this.secondary = const []});

  final List<double> values;
  final List<double> secondary;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return SizedBox(
      height: 80,
      width: double.infinity,
      child: CustomPaint(
        painter: _SparkPainter(
          values: values,
          secondary: secondary,
          primary: theme.colorScheme.primary,
          secondaryColor: theme.colorScheme.secondary,
          axis: context.hairline,
        ),
      ),
    );
  }
}

class _SparkPainter extends CustomPainter {
  _SparkPainter({
    required this.values,
    required this.secondary,
    required this.primary,
    required this.secondaryColor,
    required this.axis,
  });

  final List<double> values;
  final List<double> secondary;
  final Color primary;
  final Color secondaryColor;
  final Color axis;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawLine(
      Offset(0, size.height),
      Offset(size.width, size.height),
      Paint()
        ..color = axis
        ..strokeWidth = 1,
    );
    if (values.length < 2) return;
    final all = [...values, ...secondary];
    final maxValue = all.reduce((a, b) => a > b ? a : b);
    final minValue = all.reduce((a, b) => a < b ? a : b);
    final span = (maxValue - minValue).clamp(1.0, double.infinity);

    void draw(List<double> series, Color color, {bool dashed = false}) {
      if (series.length < 2) return;
      final path = Path();
      for (var i = 0; i < series.length; i++) {
        final x = size.width * i / (series.length - 1);
        final y = size.height -
            (series[i] - minValue) / span * (size.height - 6) -
            3;
        if (i == 0) {
          path.moveTo(x, y);
        } else {
          path.lineTo(x, y);
        }
      }
      final paint = Paint()
        ..color = color
        ..strokeWidth = dashed ? 1.5 : 2
        ..style = PaintingStyle.stroke
        ..strokeJoin = StrokeJoin.round
        ..strokeCap = StrokeCap.round;
      canvas.drawPath(path, paint);
    }

    draw(secondary, secondaryColor, dashed: true);
    draw(values, primary);
  }

  @override
  bool shouldRepaint(covariant _SparkPainter old) =>
      old.values != values ||
      old.secondary != secondary ||
      old.primary != primary;
}

class SettingsRow extends StatelessWidget {
  const SettingsRow({
    super.key,
    required this.title,
    this.value,
    this.onTap,
    this.trailing,
    this.subtitle,
    this.danger = false,
  });

  final String title;
  final String? value;
  final String? subtitle;
  final VoidCallback? onTap;
  final Widget? trailing;
  final bool danger;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final content = Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
      child: Row(
        children: [
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  title,
                  style: theme.textTheme.bodyMedium?.copyWith(
                    fontWeight: FontWeight.w500,
                    color: danger ? theme.colorScheme.error : null,
                  ),
                ),
                if (subtitle != null)
                  Padding(
                    padding: const EdgeInsets.only(top: 3),
                    child: Text(
                      subtitle!,
                      style: theme.textTheme.bodySmall
                          ?.copyWith(color: context.mutedColor),
                    ),
                  ),
              ],
            ),
          ),
          if (value != null)
            Text(
              value!,
              style: theme.textTheme.bodySmall?.copyWith(
                color: context.mutedColor,
                fontFeatures: const [FontFeature.tabularFigures()],
              ),
            ),
          if (trailing != null) ...[const SizedBox(width: 8), trailing!],
          if (onTap != null && trailing == null)
            Icon(Icons.chevron_right_rounded, size: 20, color: context.mutedColor),
        ],
      ),
    );
    if (onTap == null) return content;
    return InkWell(onTap: onTap, child: content);
  }
}
