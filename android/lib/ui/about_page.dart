import 'package:flutter/material.dart';

import '../theme/app_theme.dart';
import 'widgets.dart';

class AboutPage extends StatelessWidget {
  const AboutPage({super.key});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      appBar: AppBar(title: const Text('关于')),
      body: ListView(
        padding: const EdgeInsets.all(kPageGutter),
        children: [
          Row(
            children: [
              Container(
                width: 56,
                height: 56,
                decoration: BoxDecoration(
                  color: AppColors.secondary,
                  borderRadius: BorderRadius.circular(16),
                ),
                alignment: Alignment.center,
                child: const Text(
                  '3DS',
                  style: TextStyle(
                    color: Colors.white,
                    fontWeight: FontWeight.w800,
                    fontSize: 14,
                  ),
                ),
              ),
              const SizedBox(width: 14),
              Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text('3DS Live', style: theme.textTheme.titleLarge),
                  Text(
                    'Android 端 · 0.1.12',
                    style: theme.textTheme.bodySmall
                        ?.copyWith(color: context.mutedColor),
                  ),
                ],
              ),
            ],
          ),
          const SizedBox(height: 20),
          Text(
            '把 New 3DS 的上下双屏和游戏立体声，经局域网无线投到手机。'
            '接收端协议与 Windows 播放器一致，掌机端无需改动。',
            style: theme.textTheme.bodyMedium?.copyWith(
              color: context.mutedColor,
            ),
          ),
          const SizedBox(height: 24),
          const NoticeBanner(
            text: '仅在同一可信局域网使用；协议与 PC 端一样没有加密。',
            icon: Icons.lan_outlined,
            tone: AppColors.warning,
          ),
          const SizedBox(height: 24),
          Container(
            decoration: BoxDecoration(
              color: theme.colorScheme.surfaceContainerLow,
              borderRadius: BorderRadius.circular(kCardRadius),
              border: Border.all(color: context.hairline),
            ),
            clipBehavior: Clip.antiAlias,
            child: Column(
              children: [
                SettingsRow(
                  title: '项目主页',
                  subtitle: 'github.com/nicedayzhu/3ds-live',
                  onTap: () {},
                ),
                Divider(height: 1, color: context.hairline),
                SettingsRow(
                  title: '开源许可',
                  subtitle: 'MIT · 第三方许可见包内 licenses',
                  onTap: () {},
                ),
                Divider(height: 1, color: context.hairline),
                SettingsRow(
                  title: '致谢',
                  subtitle: 'BolgarMaxym97 / 3ds-recorder、libctru、LZ4、Qt 社区',
                  onTap: () {},
                ),
              ],
            ),
          ),
          const SizedBox(height: 24),
          Text(
            '本应用不包含游戏画面、密钥或任天堂素材；画面来自用户自己的掌机。',
            style: theme.textTheme.bodySmall?.copyWith(
              fontSize: 11,
              color: context.mutedColor,
            ),
          ),
        ],
      ),
    );
  }
}
