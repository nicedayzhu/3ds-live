import 'package:flutter/material.dart';

import 'stream/live_controller.dart';
import 'theme/app_theme.dart';
import 'ui/app_shell.dart';
import 'ui/widgets.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const ThreeDsLiveApp());
}

class ThreeDsLiveApp extends StatefulWidget {
  const ThreeDsLiveApp({super.key});

  @override
  State<ThreeDsLiveApp> createState() => _ThreeDsLiveAppState();
}

class _ThreeDsLiveAppState extends State<ThreeDsLiveApp> {
  static const bool _mock = bool.fromEnvironment('THREEDS_MOCK');
  final LiveController _controller = LiveController(mock: _mock);
  ThemeMode _themeMode = ThemeMode.system;

  @override
  void initState() {
    super.initState();
    _controller.initialize();
  }

  void _toggleTheme() {
    final systemDark =
        WidgetsBinding.instance.platformDispatcher.platformBrightness ==
            Brightness.dark;
    final dark = _themeMode == ThemeMode.dark ||
        (_themeMode == ThemeMode.system && systemDark);
    setState(() {
      _themeMode = dark ? ThemeMode.light : ThemeMode.dark;
    });
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return LiveScope(
      controller: _controller,
      child: MaterialApp(
        title: '3DS Live',
        debugShowCheckedModeBanner: false,
        theme: buildAppTheme(Brightness.light),
        darkTheme: buildAppTheme(Brightness.dark),
        themeMode: _themeMode,
        home: AppShell(
          onToggleTheme: _toggleTheme,
          themeMode: _themeMode,
        ),
      ),
    );
  }
}
