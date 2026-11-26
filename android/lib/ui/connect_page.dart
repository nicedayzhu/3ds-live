import 'dart:async';

import 'package:flutter/material.dart';

import '../stream/models.dart';
import '../theme/app_theme.dart';
import 'widgets.dart';

class ConnectPage extends StatefulWidget {
  const ConnectPage({
    super.key,
    required this.onOpenLive,
    this.onOpenSettings,
    this.onToggleTheme,
  });

  final VoidCallback onOpenLive;
  final VoidCallback? onOpenSettings;
  final VoidCallback? onToggleTheme;

  @override
  State<ConnectPage> createState() => _ConnectPageState();
}

class _ConnectPageState extends State<ConnectPage> {
  bool _guideExpanded = true;
  Timer? _slowSearchTimer;
  bool _showBusyHint = false;

  @override
  void dispose() {
    _slowSearchTimer?.cancel();
    super.dispose();
  }

  void _startSearch() {
    final controller = context.live;
    controller.start();
    _slowSearchTimer?.cancel();
    setState(() => _showBusyHint = false);
    _slowSearchTimer = Timer(const Duration(seconds: 8), () {
      if (!mounted) return;
      if (controller.phase == StreamPhase.live) return;
      setState(() => _showBusyHint = true);
    });
  }

  Future<void> _manualIpDialog() async {
    final controller = context.live;
    final text = TextEditingController(text: controller.settings.handheldIp);
    final ip = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('手动填写掌机 IPv4'),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              '留空表示自动发现。填写后只接受该地址的掌机。',
              style: Theme.of(context).textTheme.bodySmall,
            ),
            const SizedBox(height: 12),
            TextField(
              controller: text,
              keyboardType: TextInputType.number,
              decoration: const InputDecoration(hintText: '例如 192.168.1.23'),
            ),
          ],
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: const Text('取消'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(context, text.text.trim()),
            child: const Text('应用'),
          ),
        ],
      ),
    );
    if (ip == null || !mounted) return;
    await controller.applySettings(
      controller.settings.copyWith(handheldIp: ip),
    );
    if (ip.isNotEmpty) {
      await controller.connectTo(Handheld(ip: ip));
    }
  }

  Future<void> _connect(Handheld handheld) async {
    final controller = context.live;
    await controller.connectTo(handheld);
    if (!mounted) return;
    if (controller.phase == StreamPhase.live ||
        controller.phase == StreamPhase.reserved) {
      widget.onOpenLive();
    }
  }

  @override
  Widget build(BuildContext context) {
    final controller = context.live;
    final theme = Theme.of(context);
    final running = controller.isRunning;
    return CustomScrollView(
      slivers: [
        SliverToBoxAdapter(
          child: BrandBar(
            subtitle: controller.mockMode ? '演示模式' : null,
            onTheme: widget.onToggleTheme,
            onSettings: widget.onOpenSettings,
          ),
        ),
        SliverPadding(
          padding: const EdgeInsets.symmetric(horizontal: kPageGutter),
          sliver: SliverList(
            delegate: SliverChildListDelegate([
              if (controller.mockMode)
                const Padding(
                  padding: EdgeInsets.only(bottom: 12),
                  child: NoticeBanner(
                    text: '未检测到原生接收引擎，界面使用演示数据。',
                    icon: Icons.science_outlined,
                    tone: AppColors.warning,
                  ),
                ),
              if (_showBusyHint && !controller.isLive)
                const Padding(
                  padding: EdgeInsets.only(bottom: 12),
                  child: NoticeBanner(
                    text: '同一时刻只有一台设备能接收。若电脑端播放器正在投流，请先在电脑上停止，再回到这里重试。',
                    icon: Icons.devices_other_rounded,
                    tone: AppColors.warning,
                  ),
                ),
              StatusCard(
                phase: controller.phase,
                message: controller.message,
                busy: controller.busy,
                primaryLabel: running ? '停止接收' : '开始寻找掌机',
                onPrimary: () {
                  if (running) {
                    _slowSearchTimer?.cancel();
                    setState(() => _showBusyHint = false);
                    controller.stop();
                  } else {
                    _startSearch();
                  }
                },
                secondaryLabel: '手动 IP…',
                onSecondary: _manualIpDialog,
              ),
              if (running)
                Padding(
                  padding: const EdgeInsets.only(top: 12),
                  child: FilledButton.tonalIcon(
                    onPressed: widget.onOpenLive,
                    icon: const Icon(Icons.play_circle_outline_rounded),
                    label: Text(
                      controller.isLive ? '进入直播画面' : '查看等待画面',
                    ),
                  ),
                ),
              SectionHeader(
                title: '发现的掌机',
                trailing: Text(
                  controller.devices.isEmpty
                      ? ''
                      : '${controller.devices.length} 台',
                  style: theme.textTheme.bodySmall
                      ?.copyWith(color: context.mutedColor),
                ),
              ),
              if (controller.devices.isEmpty)
                EmptyState(
                  title: '还没有发现掌机',
                  body: '让掌机停留在「准备就绪」画面，再按一次 R + SELECT。'
                      '\n也可以手动填写掌机的 IPv4。',
                  actionLabel: '手动 IP…',
                  onAction: _manualIpDialog,
                )
              else
                ...controller.devices.map(
                  (handheld) => DeviceCard(
                    handheld: handheld,
                    onTap: () => _connect(handheld),
                  ),
                ),
              if (controller.recent != null) ...[
                const SectionHeader(title: '最近使用'),
                DeviceCard(
                  handheld: controller.recent!,
                  subtitle:
                      '${controller.recent!.ip} · ${qualityLabel(controller.settings.jpegQuality)} · ${controller.settings.fps} FPS',
                  onTap: () => _connect(controller.recent!),
                ),
              ],
              const SizedBox(height: 24),
              _GuideCard(
                expanded: _guideExpanded,
                onToggle: () => setState(() => _guideExpanded = !_guideExpanded),
              ),
              const SizedBox(height: 12),
              const NoticeBanner(
                text: '手机与掌机需在同一 Wi-Fi；5 GHz 或 Wi-Fi 6 更稳定，避免访客网络与 AP 隔离。',
              ),
              const SizedBox(height: 24),
            ]),
          ),
        ),
      ],
    );
  }


}

class _GuideCard extends StatelessWidget {
  const _GuideCard({required this.expanded, required this.onToggle});

  final bool expanded;
  final VoidCallback onToggle;

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
          InkWell(
            onTap: onToggle,
            child: Row(
              children: [
                Text('首次使用？3 步开始投屏',
                    style: theme.textTheme.titleMedium),
                const Spacer(),
                Icon(
                  expanded ? Icons.expand_less_rounded : Icons.expand_more_rounded,
                  size: 20,
                  color: context.mutedColor,
                ),
              ],
            ),
          ),
          AnimatedCrossFade(
            firstChild: const SizedBox(width: double.infinity),
            secondChild: Column(
              children: [
                for (var i = 0; i < 3; i++)
                  Padding(
                    padding: const EdgeInsets.only(top: 10),
                    child: Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Container(
                          width: 20,
                          height: 20,
                          decoration: BoxDecoration(
                            color: theme.colorScheme.surfaceContainerHigh,
                            shape: BoxShape.circle,
                          ),
                          alignment: Alignment.center,
                          child: Text(
                            '${i + 1}',
                            style: theme.textTheme.labelSmall?.copyWith(
                              fontWeight: FontWeight.w700,
                            ),
                          ),
                        ),
                        const SizedBox(width: 10),
                        Expanded(
                          child: Text(
                            const [
                              '在掌机打开 3DS Live',
                              '在就绪画面按 R + SELECT',
                              '回到此页等待自动连接',
                            ][i],
                            style: theme.textTheme.bodySmall
                                ?.copyWith(color: context.mutedColor),
                          ),
                        ),
                      ],
                    ),
                  ),
              ],
            ),
            crossFadeState: expanded
                ? CrossFadeState.showSecond
                : CrossFadeState.showFirst,
            duration: const Duration(milliseconds: 180),
          ),
        ],
      ),
    );
  }
}
