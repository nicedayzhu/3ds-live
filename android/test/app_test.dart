import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:three_ds_live/stream/live_controller.dart';
import 'package:three_ds_live/stream/models.dart';
import 'package:three_ds_live/theme/app_theme.dart';
import 'package:three_ds_live/ui/connect_page.dart';
import 'package:three_ds_live/ui/diagnostics_page.dart';
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
    expect(ids, ['hd30', 'balanced30', 'low24']);
    expect(kStreamPresets.first.fps, 30);
    expect(kStreamPresets.first.jpegQuality, 80);
    expect(kStreamPresets.last.rate, 1000000);
    expect(qualityLabel(0), '自动');
    expect(qualityLabel(70), 'Q70');
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
}
