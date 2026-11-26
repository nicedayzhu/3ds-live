import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:three_ds_live/stream/live_controller.dart';
import 'package:three_ds_live/stream/models.dart';
import 'package:three_ds_live/theme/app_theme.dart';
import 'package:three_ds_live/update_check.dart';
import 'package:three_ds_live/ui/connect_page.dart';
import 'package:three_ds_live/ui/diagnostics_page.dart';
import 'package:three_ds_live/ui/live_page.dart';
import 'package:three_ds_live/ui/quality_sheet.dart';
import 'package:three_ds_live/ui/widgets.dart';

Widget _wrap(LiveController controller, Widget child) {
  return LiveScope(
    controller: controller,
    child: MaterialApp(
      theme: buildAppTheme(Brightness.light),
      home: Scaffold(body: child),
    ),
  );
}

Future<void> _phoneViewport(WidgetTester tester) async {
  await tester.binding.setSurfaceSize(const Size(412, 915));
  addTearDown(() => tester.binding.setSurfaceSize(null));
}

void main() {
  test('stream presets match the Qt player', () {
    final ids = kStreamPresets.map((preset) => preset.id).toList();
    expect(ids, ['hd30', 'balanced30', 'low24', 'saver30']);
    expect(kStreamPresets.first.fps, 30);
    expect(kStreamPresets.first.jpegQuality, 80);
    expect(kStreamPresets.last.rate, 800000);
    expect(qualityLabel(0), '自动');
    expect(qualityLabel(70), 'Q70');
  });

  test('update versions compare numerically', () {
    expect(isNewerVersion('0.1.13', '0.1.14'), isTrue);
    expect(isNewerVersion('0.1.13', '0.2.0'), isTrue);
    expect(isNewerVersion('0.1.13', '0.1.13'), isFalse);
    expect(isNewerVersion('0.1.13', '0.1.9'), isFalse);
  });

  test('stats parse the native event payload', () {
    final stats = StreamStats.fromJson(const {
      'complete_fps': 29.8,
      'top_fps': 29.7,
      'quality': 70,
      'cap_bytes': 2000000,
      'received_bytes_per_second': 1780000.0,
      'window_seconds': 4.0,
      'events': ['自适应降档'],
    });
    expect(stats.completeFps, 29.8);
    expect(stats.quality, 70);
    expect(stats.windowSeconds, 4);
    expect(stats.events.single, '自适应降档');
    expect(stats.hasData, isTrue);
  });

  testWidgets('connect page renders the status card and guide', (tester) async {
    await _phoneViewport(tester);
    final controller = LiveController(mock: true);
    await controller.initialize();
    await tester.pumpWidget(_wrap(controller, ConnectPage(onOpenLive: () {})));

    expect(find.text('开始寻找掌机'), findsOneWidget);
    expect(find.text('还没有发现掌机'), findsOneWidget);
    expect(find.text('演示模式'), findsOneWidget);

    await tester.scrollUntilVisible(
      find.text('首次使用？3 步开始投屏'),
      200,
      scrollable: find.byType(Scrollable).first,
    );
    expect(find.text('首次使用？3 步开始投屏'), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
    controller.dispose();
  });

  testWidgets('connect page wires theme and settings actions', (tester) async {
    await _phoneViewport(tester);
    final controller = LiveController(mock: true);
    await controller.initialize();
    var themes = 0;
    var settings = 0;
    await tester.pumpWidget(_wrap(
      controller,
      ConnectPage(
        onOpenLive: () {},
        onToggleTheme: () => themes++,
        onOpenSettings: () => settings++,
      ),
    ));

    await tester.tap(find.byTooltip('切换深浅色'));
    await tester.tap(find.byTooltip('设置'));
    expect(themes, 1);
    expect(settings, 1);

    await tester.pumpWidget(const SizedBox());
    controller.dispose();
  });

  testWidgets('mock session discovers devices and reaches live', (tester) async {
    await _phoneViewport(tester);
    final controller = LiveController(mock: true);
    await controller.initialize();
    await tester.pumpWidget(_wrap(controller, ConnectPage(onOpenLive: () {})));

    await controller.start();
    await tester.pump();
    expect(controller.phase, StreamPhase.inviting);
    expect(controller.devices.length, 2);

    await tester.pump(const Duration(seconds: 2));
    expect(controller.phase, StreamPhase.live);
    expect(controller.stats.completeFps, greaterThan(20));

    await tester.pumpWidget(const SizedBox());
    controller.dispose();
  });

  testWidgets('diagnostics page shows an empty state before any stream',
      (tester) async {
    await _phoneViewport(tester);
    final controller = LiveController(mock: true);
    await controller.initialize();
    await tester.pumpWidget(_wrap(controller, const DiagnosticsPage()));

    expect(find.text('暂无诊断数据'), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
    controller.dispose();
  });

  testWidgets('quality sheet lists the presets and applies a draft',
      (tester) async {
    await _phoneViewport(tester);
    final controller = LiveController(mock: true);
    await controller.initialize();
    await tester.pumpWidget(
      _wrap(
        controller,
        Builder(
          builder: (context) => FilledButton(
            onPressed: () => showQualitySheet(context),
            child: const Text('打开画质'),
          ),
        ),
      ),
    );

    await tester.tap(find.text('打开画质'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));

    expect(find.text('高清'), findsOneWidget);
    expect(find.text('均衡'), findsOneWidget);
    expect(find.text('低带宽'), findsOneWidget);
    expect(find.text('应用'), findsOneWidget);

    // Selecting 高清 makes it the active preset in the draft.
    await tester.tap(find.text('高清'));
    await tester.pump();
    expect(controller.settings.jpegQuality, 0); // not applied yet
    await tester.tap(find.text('应用'));
    await tester.pump(const Duration(milliseconds: 100));
    expect(controller.settings.jpegQuality, 80);
    expect(controller.settings.adaptiveRate, isFalse);

    await tester.pumpWidget(const SizedBox());
    controller.dispose();
  });

  testWidgets('live page hides its controls and restores them on tap',
      (tester) async {
    await _phoneViewport(tester);
    final controller = LiveController(mock: true);
    await controller.initialize();
    await controller.start();
    await tester.pump(const Duration(seconds: 2));

    await tester.pumpWidget(
      LiveScope(
        controller: controller,
        child: MaterialApp(
          theme: buildAppTheme(Brightness.dark),
          home: const LivePage(),
        ),
      ),
    );
    await tester.pump();

    expect(find.text('停止'), findsOneWidget);
    expect(
      tester.widgetList<AnimatedOpacity>(find.byType(AnimatedOpacity)).any((w) => w.opacity == 1),
      isTrue,
    );

    await tester.pump(const Duration(seconds: 4));
    expect(
      tester.widgetList<AnimatedOpacity>(find.byType(AnimatedOpacity)).any((w) => w.opacity == 0),
      isTrue,
    );

    // A single tap waits for the double-tap window before it fires.
    await tester.tapAt(const Offset(206, 500));
    await tester.pump(const Duration(milliseconds: 400));
    await tester.pump(const Duration(milliseconds: 250));
    expect(
      tester.widgetList<AnimatedOpacity>(find.byType(AnimatedOpacity)).any((w) => w.opacity == 1),
      isTrue,
    );

    await tester.pumpWidget(const SizedBox());
    controller.stop();
    controller.dispose();
  });
}
