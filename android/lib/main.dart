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
  final LiveController _controller = LiveController();
  ThemeMode _themeMode = ThemeMode.system;

  @override
  void initState() {
    super.initState();
    _controller.initialize();
  }

  void _cycleTheme() {
    setState(() {
      _themeMode = switch (_themeMode) {
        ThemeMode.system => ThemeMode.light,
        ThemeMode.light => ThemeMode.dark,
        ThemeMode.dark => ThemeMode.system,
      };
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
          onToggleTheme: _cycleTheme,
          themeMode: _themeMode,
        ),
      ),
    );
  }
}
