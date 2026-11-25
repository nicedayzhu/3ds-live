import 'package:flutter/material.dart';

import '../stream/models.dart';
import '../theme/app_theme.dart';
import 'about_page.dart';
import 'widgets.dart';

class SettingsPage extends StatelessWidget {
  const SettingsPage({
    super.key,
    required this.onToggleTheme,
    required this.themeMode,
  });

  final VoidCallback onToggleTheme;
  final ThemeMode themeMode;

  @override
  Widget build(BuildContext context) {
    final controller = context.live;
    final settings = controller.settings;
    final theme = Theme.of(context);

    Future<void> update(StreamSettings next) async {
      await controller.applySettings(next);
    }

    return ListView(
      padding: const EdgeInsets.fromLTRB(0, 8, 0, 32),
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(kPageGutter, 0, kPageGutter, 8),
          child: Text('设置', style: theme.textTheme.headlineSmall),
        ),
        _Section(title: '网络', children: [
          SettingsRow(
            title: '监听端口',
            subtitle: '掌机与播放器需一致，默认 8001',
            value: '${settings.port}',
            onTap: () async {
              final value = await _numberDialog(
                context,
                title: '监听端口',
                initial: settings.port,
                min: 1024,
                max: 65535,
              );
              if (value != null) {
                await update(settings.copyWith(port: value));
              }
            },
          ),
          SettingsRow(
            title: '自动发现',
            subtitle: '关闭后只接受下方填写的掌机地址',
            trailing: Switch(
              value: settings.autoDiscover,
              onChanged: (value) =>
                  update(settings.copyWith(autoDiscover: value)),
            ),
          ),
          SettingsRow(
            title: '掌机 IPv4',
            subtitle: '留空表示自动发现',
            value: settings.handheldIp.isEmpty ? '自动' : settings.handheldIp,
            onTap: () async {
              final value = await _textDialog(
                context,
                title: '掌机 IPv4',
                initial: settings.handheldIp,
                hint: '留空表示自动发现',
              );
              if (value != null && _validIp(value)) {
                await update(settings.copyWith(
                  handheldIp: value,
                  autoDiscover: value.isEmpty,
                ));
              }
            },
          ),
        ]),
        _Section(title: '显示', children: [
          SettingsRow(
            title: '画面布局',
            value: settings.layout.label,
            onTap: () async {
              final layout = await _layoutDialog(context, settings.layout);
              if (layout != null) {
                await update(settings.copyWith(layout: layout));
              }
            },
          ),
          _SegmentRow<int>(
            title: '播放缓冲',
            subtitle: '更大更稳，但延迟更高',
            values: const [60, 100, 160],
            selected: settings.bufferMs,
            label: (value) => '$value ms',
            onChanged: (value) =>
                update(settings.copyWith(bufferMs: value)),
          ),
          SettingsRow(
            title: '保持屏幕常亮',
            trailing: Switch(
              value: settings.keepAwake,
              onChanged: (value) => update(settings.copyWith(keepAwake: value)),
            ),
          ),
          SettingsRow(
            title: '隐藏系统状态栏',
            trailing: Switch(
              value: settings.hideSystemUi,
              onChanged: (value) =>
                  update(settings.copyWith(hideSystemUi: value)),
            ),
          ),
          SettingsRow(
            title: '外观',
            value: switch (themeMode) {
              ThemeMode.system => '跟随系统',
              ThemeMode.light => '浅色',
              ThemeMode.dark => '深色',
            },
            onTap: onToggleTheme,
          ),
        ]),
        _Section(title: '音频', children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 12, 16, 4),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('输出音量', style: theme.textTheme.bodyMedium),
                Row(
                  children: [
                    const Icon(Icons.volume_down_rounded, size: 18),
                    Expanded(
                      child: Slider(
                        value: settings.volume,
                        onChanged: (value) =>
                            update(settings.copyWith(volume: value)),
                      ),
                    ),
                    Text('${(settings.volume * 100).round()}%',
                        style: theme.textTheme.bodySmall),
                  ],
                ),
              ],
            ),
          ),
          SettingsRow(
            title: '静音',
            trailing: Switch(
              value: settings.muted,
              onChanged: (value) => update(settings.copyWith(muted: value)),
            ),
          ),
        ]),
        _Section(title: '录制', children: [
          _SegmentRow<String>(
            title: '分辨率',
            values: const ['720p', '1080p'],
            selected: settings.recordResolution,
            label: (value) => value,
            onChanged: (value) =>
                update(settings.copyWith(recordResolution: value)),
          ),
          SettingsRow(
            title: '视频码率',
            value: '${(settings.recordBitrate / 1000000).round()} Mbps',
            onTap: () async {
              final value = await _choiceDialog<int>(
                context,
                title: '视频码率',
                options: const [4000000, 8000000, 12000000],
                selected: settings.recordBitrate,
                label: (value) => '${(value / 1000000).round()} Mbps',
              );
              if (value != null) {
                await update(settings.copyWith(recordBitrate: value));
              }
            },
          ),
        ]),
        _Section(title: '高级', children: [
          SettingsRow(
            title: '清除最近使用的掌机',
            danger: true,
            onTap: () {
              controller.clearRecent();
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('已清除最近使用记录')),
              );
            },
          ),
          SettingsRow(
            title: '关于 3DS Live',
            onTap: () => Navigator.of(context).push(
              MaterialPageRoute<void>(builder: (_) => const AboutPage()),
            ),
          ),
        ]),
        Padding(
          padding: const EdgeInsets.fromLTRB(kPageGutter, 16, kPageGutter, 0),
          child: Text(
            '设置目前保存在本次运行中；接入原生引擎后会写入应用私有存储。',
            style: theme.textTheme.bodySmall?.copyWith(
              fontSize: 11,
              color: context.mutedColor,
            ),
          ),
        ),
      ],
    );
  }

  static bool _validIp(String value) {
    if (value.isEmpty) return true;
    final parts = value.split('.');
    if (parts.length != 4) return false;
    for (final part in parts) {
      final number = int.tryParse(part);
      if (number == null || number < 0 || number > 255) return false;
    }
    return true;
  }
}

class _Section extends StatelessWidget {
  const _Section({required this.title, required this.children});

  final String title;
  final List<Widget> children;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Padding(
      padding: const EdgeInsets.fromLTRB(kPageGutter, 16, kPageGutter, 0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Padding(
            padding: const EdgeInsets.only(bottom: 8),
            child: Text(
              title,
              style: theme.textTheme.labelSmall?.copyWith(
                color: context.mutedColor,
                letterSpacing: 0.4,
              ),
            ),
          ),
          Container(
            decoration: BoxDecoration(
              color: theme.colorScheme.surfaceContainerLow,
              borderRadius: BorderRadius.circular(kCardRadius),
              border: Border.all(color: context.hairline),
            ),
            clipBehavior: Clip.antiAlias,
            child: Column(
              children: [
                for (var i = 0; i < children.length; i++) ...[
                  if (i > 0)
                    Divider(height: 1, thickness: 1, color: context.hairline),
                  children[i],
                ],
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _SegmentRow<T> extends StatelessWidget {
  const _SegmentRow({
    required this.title,
    required this.values,
    required this.selected,
    required this.label,
    required this.onChanged,
    this.subtitle,
  });

  final String title;
  final String? subtitle;
  final List<T> values;
  final T selected;
  final String Function(T) label;
  final ValueChanged<T> onChanged;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(title, style: theme.textTheme.bodyMedium),
          if (subtitle != null)
            Padding(
              padding: const EdgeInsets.only(top: 3),
              child: Text(
                subtitle!,
                style: theme.textTheme.bodySmall
                    ?.copyWith(color: context.mutedColor),
              ),
            ),
          const SizedBox(height: 10),
          SizedBox(
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
          ),
        ],
      ),
    );
  }
}

Future<int?> _numberDialog(
  BuildContext context, {
  required String title,
  required int initial,
  int? min,
  int? max,
}) async {
  final text = TextEditingController(text: '$initial');
  return showDialog<int>(
    context: context,
    builder: (context) => AlertDialog(
      title: Text(title),
      content: TextField(
        controller: text,
        keyboardType: TextInputType.number,
        autofocus: true,
        decoration: InputDecoration(hintText: '$initial'),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: const Text('取消'),
        ),
        FilledButton(
          onPressed: () {
            final value = int.tryParse(text.text.trim());
            if (value == null) return;
            if (min != null && value < min) return;
            if (max != null && value > max) return;
            Navigator.pop(context, value);
          },
          child: const Text('应用'),
        ),
      ],
    ),
  );
}

Future<String?> _textDialog(
  BuildContext context, {
  required String title,
  required String initial,
  String? hint,
}) async {
  final text = TextEditingController(text: initial);
  return showDialog<String>(
    context: context,
    builder: (context) => AlertDialog(
      title: Text(title),
      content: TextField(
        controller: text,
        keyboardType: TextInputType.number,
        autofocus: true,
        decoration: InputDecoration(hintText: hint),
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
}

Future<T?> _choiceDialog<T>(
  BuildContext context, {
  required String title,
  required List<T> options,
  required T selected,
  required String Function(T) label,
}) {
  return showDialog<T>(
    context: context,
    builder: (context) => SimpleDialog(
      title: Text(title),
      children: [
        for (final option in options)
          ListTile(
            title: Text(label(option)),
            trailing: option == selected
                ? Icon(Icons.check_rounded,
                    color: Theme.of(context).colorScheme.primary)
                : null,
            onTap: () => Navigator.pop(context, option),
          ),
      ],
    ),
  );
}

Future<VideoLayout?> _layoutDialog(
  BuildContext context,
  VideoLayout current,
) {
  return showDialog<VideoLayout>(
    context: context,
    builder: (context) => SimpleDialog(
      title: const Text('画面布局'),
      children: [
        for (final layout in VideoLayout.values)
          ListTile(
            title: Text(layout.label),
            trailing: layout == current
                ? Icon(Icons.check_rounded,
                    color: Theme.of(context).colorScheme.primary)
                : null,
            onTap: () => Navigator.pop(context, layout),
          ),
      ],
    ),
  );
}
