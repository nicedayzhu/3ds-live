import 'package:flutter/material.dart';

import '../stream/models.dart';
import '../theme/app_theme.dart';
import 'widgets.dart';

class DiagnosticsPage extends StatelessWidget {
  const DiagnosticsPage({
    super.key,
    this.standalone = false,
    this.onOpenSettings,
    this.onToggleTheme,
  });

  final bool standalone;
  final VoidCallback? onOpenSettings;
  final VoidCallback? onToggleTheme;

  @override
  Widget build(BuildContext context) {
    final body = SafeArea(
      bottom: false,
      child: ListView(
        padding: const EdgeInsets.fromLTRB(kPageGutter, 8, kPageGutter, 32),
        children: [
          if (standalone)
            Row(
              children: [
                IconButton(
                  onPressed: () => Navigator.of(context).pop(),
                  icon: const Icon(Icons.arrow_back_rounded),
                  tooltip: '返回',
                ),
                const Spacer(),
              ],
            ),
          _DiagnosticsBody(
            onOpenSettings: onOpenSettings,
            onToggleTheme: onToggleTheme,
          ),
        ],
      ),
    );
    if (!standalone) return body;
    return Scaffold(
      body: body,
    );
  }
}

class _DiagnosticsBody extends StatelessWidget {
  const _DiagnosticsBody({this.onOpenSettings, this.onToggleTheme});

  final VoidCallback? onOpenSettings;
  final VoidCallback? onToggleTheme;

  @override
  Widget build(BuildContext context) {
    final controller = context.live;
    final theme = Theme.of(context);
    final stats = controller.stats;

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            Text('诊断', style: theme.textTheme.headlineSmall),
            const Spacer(),
            if (onToggleTheme != null)
              IconButton(
                tooltip: '切换深浅色',
                onPressed: onToggleTheme,
                icon: const Icon(Icons.contrast_rounded),
              ),
            if (onOpenSettings != null)
              IconButton(
                tooltip: '设置',
                onPressed: onOpenSettings,
                icon: const Icon(Icons.tune_rounded),
              ),
            TextButton.icon(
              onPressed: () async {
                final data = await controller.exportDiagnostics();
                if (!context.mounted) return;
                ScaffoldMessenger.of(context).showSnackBar(
                  SnackBar(
                    content: Text(
                      data == null ? '导出失败' : '已生成诊断快照（${data.length} 个字段）',
                    ),
                  ),
                );
              },
              icon: const Icon(Icons.ios_share_rounded, size: 18),
              label: const Text('导出 JSON'),
            ),
          ],
        ),
        const SizedBox(height: 4),
        Text(
          stats.windowSeconds > 0
              ? '采样窗口 ${stats.windowSeconds.toStringAsFixed(1)} s · 已对齐两端状态报文'
              : '开始投流后这里会显示两端对齐的统计',
          style: theme.textTheme.bodySmall?.copyWith(color: context.mutedColor),
        ),
        const SizedBox(height: 16),
        if (!stats.hasData)
          EmptyState(
            title: '暂无诊断数据',
            body: controller.isRunning
                ? '正在等待第一对完整画面与掌机状态报文。'
                : '先连接掌机并开始投流，指标会按秒刷新。',
            icon: Icons.monitor_heart_outlined,
            actionLabel: '回到连接',
            onAction: () => Navigator.of(context).maybePop(),
          )
        else ...[
          GridView.count(
            crossAxisCount: 3,
            mainAxisSpacing: 8,
            crossAxisSpacing: 8,
            childAspectRatio: 121 / 84,
            shrinkWrap: true,
            physics: const NeverScrollableScrollPhysics(),
            children: [
              MetricTile(
                label: '双屏收齐',
                value: formatFps(stats.completeFps),
                unit: 'FPS',
              ),
              MetricTile(
                label: '上屏呈现',
                value: formatFps(stats.topFps),
                unit: 'FPS',
              ),
              MetricTile(
                label: '掌机上限',
                value: stats.capBytes > 0
                    ? (stats.capBytes / 1000000).toStringAsFixed(2)
                    : '—',
                unit: 'MB/s',
              ),
              MetricTile(
                label: '掌机提交',
                value: formatBytesPerSecond(stats.sentBytesPerSecond),
              ),
              MetricTile(
                label: '本机实收',
                value: formatBytesPerSecond(stats.receivedBytesPerSecond),
              ),
              MetricTile(
                label: '实际质量',
                value: qualityLabel(stats.quality),
              ),
            ],
          ),
          const SizedBox(height: 12),
          _ChartCard(history: controller.history),
          const SizedBox(height: 12),
          _RowsCard(stats: stats),
          if (stats.events.isNotEmpty) ...[
            const SizedBox(height: 12),
            _EventsCard(events: stats.events),
          ],
          const SizedBox(height: 12),
          Text(
            '窗口以掌机状态报文对齐：两端 MB/s 同计音视频与 IPv4/UDP 头，排除状态报文。',
            style: theme.textTheme.bodySmall?.copyWith(
              fontSize: 11,
              color: context.mutedColor,
            ),
          ),
        ],
      ],
    );
  }
}

class _ChartCard extends StatelessWidget {
  const _ChartCard({required this.history});

  final List<StreamStats> history;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final fps = history.map((s) => s.completeFps).toList();
    final bandwidth = history
        .map((s) => s.receivedBytesPerSecond / 1000000)
        .toList();
    return Container(
      padding: const EdgeInsets.fromLTRB(14, 14, 14, 10),
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
              Text('近 60 秒', style: theme.textTheme.titleMedium),
              const Spacer(),
              _Legend(color: theme.colorScheme.primary, label: '收齐 FPS'),
              const SizedBox(width: 10),
              _Legend(color: theme.colorScheme.secondary, label: '带宽'),
            ],
          ),
          const SizedBox(height: 10),
          Sparkline(values: fps, secondary: bandwidth),
          const SizedBox(height: 4),
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text('60 秒前',
                  style: theme.textTheme.labelSmall
                      ?.copyWith(color: context.mutedColor)),
              Text('现在',
                  style: theme.textTheme.labelSmall
                      ?.copyWith(color: context.mutedColor)),
            ],
          ),
        ],
      ),
    );
  }
}

class _Legend extends StatelessWidget {
  const _Legend({required this.color, required this.label});

  final Color color;
  final String label;

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Container(
          width: 12,
          height: 2,
          color: color,
        ),
        const SizedBox(width: 4),
        Text(
          label,
          style: Theme.of(context)
              .textTheme
              .labelSmall
              ?.copyWith(color: context.mutedColor),
        ),
      ],
    );
  }
}

class _RowsCard extends StatelessWidget {
  const _RowsCard({required this.stats});

  final StreamStats stats;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final rows = [
      ('缺片帧', '${stats.droppedPairs}'),
      ('修复成功 / 超时', '${stats.repairedPairs} / ${stats.repairTimeouts}'),
      ('行复用节省', '${stats.rowSavingsPercent.toStringAsFixed(0)}%'),
      ('LZ4 下屏节省', '${stats.lz4SavingsPercent.toStringAsFixed(0)}%'),
      ('音频已收 / 空洞',
          '${stats.audioSeconds.toStringAsFixed(1)} s / ${stats.audioGapSeconds.toStringAsFixed(1)} s'),
      ('播放缓冲', '${stats.bufferMs.toStringAsFixed(0)} ms'),
    ];
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
      decoration: BoxDecoration(
        color: theme.colorScheme.surfaceContainerLow,
        borderRadius: BorderRadius.circular(kCardRadius),
        border: Border.all(color: context.hairline),
      ),
      child: Column(
        children: [
          for (var i = 0; i < rows.length; i++)
            Container(
              padding: const EdgeInsets.symmetric(vertical: 12),
              decoration: BoxDecoration(
                border: i == 0
                    ? null
                    : Border(top: BorderSide(color: context.hairline)),
              ),
              child: Row(
                children: [
                  Text(rows[i].$1,
                      style: theme.textTheme.bodyMedium
                          ?.copyWith(color: context.mutedColor)),
                  const Spacer(),
                  Text(rows[i].$2,
                      style: theme.textTheme.bodyMedium
                          ?.copyWith(fontWeight: FontWeight.w600)),
                ],
              ),
            ),
        ],
      ),
    );
  }
}

class _EventsCard extends StatelessWidget {
  const _EventsCard({required this.events});

  final List<String> events;

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
          Text('最近事件', style: theme.textTheme.titleMedium),
          const SizedBox(height: 8),
          for (final event in events.take(3))
            Padding(
              padding: const EdgeInsets.only(top: 4),
              child: Row(
                children: [
                  Icon(Icons.bolt_rounded, size: 14, color: context.mutedColor),
                  const SizedBox(width: 6),
                  Expanded(
                    child: Text(
                      event,
                      style: theme.textTheme.bodySmall
                          ?.copyWith(color: context.mutedColor),
                    ),
                  ),
                ],
              ),
            ),
        ],
      ),
    );
  }
}
