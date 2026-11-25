import 'package:flutter/material.dart';

/// Design tokens for 3DS Live Android.
///
/// Derived from `cia-bridge/lib/app_theme.dart` (the baseline named in
/// docs/android/UI-SPEC.md) and scaled for touch: 16 dp page gutters, 56 dp
/// settings rows, 48 dp controls, one teal action colour.
abstract final class AppColors {
  static const ink = Color(0xFF1C2030);
  static const canvas = Color(0xFFF7F8FC);
  static const surface = Colors.white;
  static const surfaceHigh = Color(0xFFF1F3F8);
  static const softSurface = Color(0xFFF3F5FA);
  static const line = Color(0xFFE7E9F2);
  static const muted = Color(0xFF73798C);
  static const primary = Color(0xFF238E82);
  static const primarySoft = Color(0xFFE7F1ED);
  static const secondary = Color(0xFF5268F2);
  static const success = Color(0xFF2CAF7A);
  static const warning = Color(0xFFE19B39);
  static const danger = Color(0xFFD65757);

  static const darkCanvas = Color(0xFF081411);
  static const darkSurface = Color(0xFF10211D);
  static const darkSurfaceHigh = Color(0xFF172C27);
  static const darkLine = Color(0xFF29413A);
  static const darkText = Color(0xFFEAF5F2);
  static const darkMuted = Color(0xFF9CB0AA);
  static const darkPrimary = Color(0xFF78D7C7);
  static const darkPrimarySoft = Color(0xFF1B2D27);
  static const darkSecondary = Color(0xFF9AA8FF);
  static const darkDanger = Color(0xFFFFB4AB);

  /// Status colours intentionally mirror the handheld notification LED.
  static const ledIdle = Color(0xFF3D7BF7);
  static const ledSearching = Color(0xFFE8A33D);
  static const ledStreaming = Color(0xFF35C07A);
  static const ledError = Color(0xFFE05A5A);
}

const double kPageGutter = 16;
const double kCardRadius = 16;
const double kControlRadius = 14;
const double kInputRadius = 12;
const double kSheetRadius = 24;
const Duration kPageAnimation = Duration(milliseconds: 220);
const Duration kQuickAnimation = Duration(milliseconds: 180);

ThemeData buildAppTheme(Brightness brightness) {
  final dark = brightness == Brightness.dark;
  final primary = dark ? AppColors.darkPrimary : AppColors.primary;
  final surface = dark ? AppColors.darkSurface : AppColors.surface;
  final onSurface = dark ? AppColors.darkText : AppColors.ink;
  final outline = dark ? AppColors.darkLine : AppColors.line;

  final scheme = ColorScheme.fromSeed(
    seedColor: const Color(0xFF48C9B0),
    brightness: brightness,
    primary: primary,
    secondary: dark ? AppColors.darkSecondary : AppColors.secondary,
    surface: surface,
    onSurface: onSurface,
    outline: outline,
    error: dark ? AppColors.darkDanger : AppColors.danger,
  );

  final base = ThemeData(
    useMaterial3: true,
    colorScheme: scheme,
    scaffoldBackgroundColor: dark ? AppColors.darkCanvas : AppColors.canvas,
    fontFamily: 'NotoSansSC',
    fontFamilyFallback: const ['Roboto', 'sans-serif'],
    visualDensity: VisualDensity.standard,
  );

  final textTheme = base.textTheme
      .apply(bodyColor: scheme.onSurface, displayColor: scheme.onSurface)
      .copyWith(
        headlineSmall: base.textTheme.headlineSmall?.copyWith(
          fontSize: 24,
          height: 32 / 24,
          fontWeight: FontWeight.w700,
          letterSpacing: -0.3,
        ),
        titleLarge: base.textTheme.titleLarge?.copyWith(
          fontSize: 17,
          height: 24 / 17,
          fontWeight: FontWeight.w700,
          letterSpacing: -0.2,
        ),
        titleMedium: base.textTheme.titleMedium?.copyWith(
          fontSize: 15,
          height: 22 / 15,
          fontWeight: FontWeight.w600,
        ),
        bodyMedium: base.textTheme.bodyMedium?.copyWith(
          fontSize: 14,
          height: 21 / 14,
        ),
        bodySmall: base.textTheme.bodySmall?.copyWith(
          fontSize: 12,
          height: 18 / 12,
        ),
        labelSmall: base.textTheme.labelSmall?.copyWith(
          fontSize: 11,
          height: 16 / 11,
          fontWeight: FontWeight.w600,
        ),
      );

  return base.copyWith(
    textTheme: textTheme,
    appBarTheme: AppBarTheme(
      elevation: 0,
      scrolledUnderElevation: 0,
      centerTitle: false,
      backgroundColor: scheme.surfaceContainerLow,
      foregroundColor: scheme.onSurface,
      surfaceTintColor: Colors.transparent,
    ),
    cardTheme: CardThemeData(
      color: scheme.surfaceContainerLow,
      elevation: 0,
      surfaceTintColor: Colors.transparent,
      margin: EdgeInsets.zero,
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.all(Radius.circular(kCardRadius)),
      ),
    ),
    inputDecorationTheme: InputDecorationTheme(
      filled: true,
      fillColor: dark ? AppColors.darkSurfaceHigh : AppColors.surfaceHigh,
      contentPadding: const EdgeInsets.symmetric(horizontal: 14, vertical: 13),
      border: OutlineInputBorder(
        borderRadius: BorderRadius.circular(kInputRadius),
        borderSide: BorderSide.none,
      ),
      focusedBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(kInputRadius),
        borderSide: BorderSide(color: scheme.primary, width: 1.5),
      ),
    ),
    filledButtonTheme: FilledButtonThemeData(
      style: FilledButton.styleFrom(
        minimumSize: const Size(0, 48),
        padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 12),
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(kControlRadius),
        ),
        elevation: 0,
        textStyle: const TextStyle(fontWeight: FontWeight.w700, fontSize: 14),
      ),
    ),
    outlinedButtonTheme: OutlinedButtonThemeData(
      style: OutlinedButton.styleFrom(
        minimumSize: const Size(0, 44),
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
        side: BorderSide(color: scheme.outlineVariant),
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(kControlRadius),
        ),
        foregroundColor: scheme.onSurface,
        textStyle: const TextStyle(fontWeight: FontWeight.w700, fontSize: 14),
      ),
    ),
    segmentedButtonTheme: SegmentedButtonThemeData(
      style: ButtonStyle(
        padding: const WidgetStatePropertyAll(
          EdgeInsets.symmetric(horizontal: 12, vertical: 10),
        ),
        shape: WidgetStatePropertyAll(
          RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(kControlRadius),
          ),
        ),
        textStyle: const WidgetStatePropertyAll(
          TextStyle(fontWeight: FontWeight.w600, fontSize: 12),
        ),
      ),
    ),
    snackBarTheme: SnackBarThemeData(
      behavior: SnackBarBehavior.floating,
      backgroundColor: scheme.inverseSurface,
      contentTextStyle: TextStyle(color: scheme.onInverseSurface),
    ),
    dialogTheme: DialogThemeData(
      backgroundColor: scheme.surfaceContainerHigh,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(20)),
    ),
    dividerTheme: DividerThemeData(color: scheme.outlineVariant, thickness: 1),
    listTileTheme: const ListTileThemeData(
      minVerticalPadding: 10,
      contentPadding: EdgeInsets.symmetric(horizontal: 16),
    ),
  );
}

extension SurfaceColors on BuildContext {
  bool get isDark => Theme.of(this).brightness == Brightness.dark;
  Color get mutedColor =>
      isDark ? AppColors.darkMuted : AppColors.muted;
  Color get softSurface =>
      isDark ? AppColors.darkSurfaceHigh : AppColors.softSurface;
  Color get hairline => isDark ? AppColors.darkLine : AppColors.line;
}
