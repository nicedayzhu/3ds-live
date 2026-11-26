import 'dart:async';

import 'package:flutter/material.dart';
import 'package:url_launcher/url_launcher.dart';

import '../stream/live_controller.dart';
import '../stream/models.dart';
import '../theme/app_theme.dart';
import '../update_check.dart';
import '../version.dart';
import 'connect_page.dart';
import 'diagnostics_page.dart';
import 'live_page.dart';
import 'settings_page.dart';
import 'widgets.dart';

class AppShell extends StatefulWidget {
  const AppShell({
    super.key,
    required this.onToggleTheme,
    required this.themeMode,
  });

  final VoidCallback onToggleTheme;
  final ThemeMode themeMode;

  @override
  State<AppShell> createState() => _AppShellState();
}

class _AppShellState extends State<AppShell> {
  final _scaffoldKey = GlobalKey<ScaffoldState>();
  int _index = 0;
  LiveController? _controller;
  StreamPhase? _lastPhase;
  UpdateInfo? _update;
  bool _updateDismissed = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _checkUpdate());
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    final controller = context.live;
    if (identical(controller, _controller)) return;
    _controller?.removeListener(_onControllerChanged);
    _controller = controller..addListener(_onControllerChanged);
  }

  @override
  void dispose() {
    _controller?.removeListener(_onControllerChanged);
    super.dispose();
  }

  void _onControllerChanged() {
    final controller = _controller;
    if (controller == null) return;
    final phase = controller.phase;
    final previous = _lastPhase;
    _lastPhase = phase;
    if (phase != StreamPhase.live || previous == StreamPhase.live) return;
    if (ModalRoute.of(context)?.isCurrent ?? false) _openLive();
  }

  Future<void> _checkUpdate() async {
    final update = await checkForUpdate();
    if (!mounted || update == null) return;
    setState(() => _update = update);
  }

  Future<void> _openRelease(UpdateInfo update) async {
    if (!await launchUrl(Uri.parse(update.url))) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('无法打开浏览器：${update.url}')),
      );
    }
  }

  void _openLive({bool replace = false}) {
    final controller = _controller;
    if (controller == null) return;
    final route = MaterialPageRoute<void>(
      builder: (_) => const LivePage(),
      fullscreenDialog: true,
    );
    if (replace) {
      Navigator.of(context).pushReplacement(route);
    } else {
      Navigator.of(context).push(route);
    }
  }

  void _openSettings() {
    _scaffoldKey.currentState?.openEndDrawer();
  }

  @override
  Widget build(BuildContext context) {
    final controller = context.live;
    final update = _update;
    return Scaffold(
      key: _scaffoldKey,
      drawerScrimColor: Colors.black.withValues(alpha: 0.55),
      endDrawer: _SettingsDrawer(
        themeMode: widget.themeMode,
        onToggleTheme: widget.onToggleTheme,
      ),
      body: SafeArea(
        bottom: false,
        child: Column(
          children: [
            if (update != null && !_updateDismissed)
              _UpdateBanner(
                update: update,
                onDismiss: () => setState(() => _updateDismissed = true),
                onOpen: () => _openRelease(update),
              ),
            Expanded(
              child: IndexedStack(
                index: _index,
                children: [
                  ConnectPage(
                    onOpenLive: _openLive,
                    onOpenSettings: _openSettings,
                    onToggleTheme: widget.onToggleTheme,
                  ),
                  DiagnosticsPage(
                    onOpenSettings: _openSettings,
                    onToggleTheme: widget.onToggleTheme,
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
      bottomNavigationBar: NavigationBar(
        height: 64,
        selectedIndex: _index,
        onDestinationSelected: (index) {
          if (index == _index) return;
          setState(() => _index = index);
        },
        destinations: const [
          NavigationDestination(
            icon: Icon(Icons.wifi_tethering_rounded),
            label: '连接',
          ),
          NavigationDestination(
            icon: Icon(Icons.monitor_heart_outlined),
            label: '诊断',
          ),
        ],
      ),
      floatingActionButton: controller.isLive && _index != 0
          ? FloatingActionButton.extended(
              onPressed: _openLive,
              backgroundColor: AppColors.ledStreaming,
              foregroundColor: Colors.white,
              icon: const Icon(Icons.play_arrow_rounded),
              label: const Text('回到直播'),
            )
          : null,
    );
  }
}

class _UpdateBanner extends StatelessWidget {
  const _UpdateBanner({
    required this.update,
    required this.onDismiss,
    required this.onOpen,
  });

  final UpdateInfo update;
  final VoidCallback onDismiss;
  final VoidCallback onOpen;

  @override
  Widget build(BuildContext context) {
    return MaterialBanner(
      content: Text('发现新版本 v${update.version}（当前 v$appVersion）'),
      leading: const Icon(Icons.system_update_alt_rounded),
      actions: [
        TextButton(onPressed: onDismiss, child: const Text('稍后')),
        FilledButton.tonal(onPressed: onOpen, child: const Text('查看更新')),
      ],
    );
  }
}

class _SettingsDrawer extends StatelessWidget {
  const _SettingsDrawer({
    required this.themeMode,
    required this.onToggleTheme,
  });

  final ThemeMode themeMode;
  final VoidCallback onToggleTheme;

  @override
  Widget build(BuildContext context) {
    final width = MediaQuery.sizeOf(context).width;
    final panelWidth = width < 600 ? width * 0.92 : 420.0;
    final theme = Theme.of(context);
    return Drawer(
      width: panelWidth,
      elevation: 24,
      backgroundColor: theme.colorScheme.surface,
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.horizontal(left: Radius.circular(24)),
      ),
      child: SafeArea(
        left: false,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(24, 16, 12, 12),
              child: Row(
                children: [
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          '设置',
                          style: theme.textTheme.headlineSmall?.copyWith(
                            fontWeight: FontWeight.w800,
                          ),
                        ),
                        Text(
                          '3DS Live · v$appVersion',
                          style: theme.textTheme.bodySmall?.copyWith(
                            color: context.mutedColor,
                          ),
                        ),
                      ],
                    ),
                  ),
                  IconButton(
                    tooltip: '关闭设置',
                    onPressed: () => Navigator.pop(context),
                    icon: const Icon(Icons.close_rounded),
                  ),
                ],
              ),
            ),
            const Divider(height: 1),
            Expanded(
              child: SettingsPage(
                showHeader: false,
                themeMode: themeMode,
                onToggleTheme: onToggleTheme,
              ),
            ),
          ],
        ),
      ),
    );
  }
}
