import 'package:flutter/material.dart';

import '../stream/live_controller.dart';
import '../stream/models.dart';
import '../theme/app_theme.dart';
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
  int _index = 0;
  LiveController? _controller;
  StreamPhase? _lastPhase;

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

  @override
  Widget build(BuildContext context) {
    final controller = context.live;
    return Scaffold(
      body: SafeArea(
        bottom: false,
        child: IndexedStack(
          index: _index,
          children: [
            ConnectPage(onOpenLive: _openLive),
            const DiagnosticsPage(),
            SettingsPage(
              onToggleTheme: widget.onToggleTheme,
              themeMode: widget.themeMode,
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
          NavigationDestination(
            icon: Icon(Icons.tune_rounded),
            label: '设置',
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
