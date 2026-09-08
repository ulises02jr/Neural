import 'package:flutter/material.dart';

/// Paleta que replica el panel web de musicos (oscuro con acento dorado).
class NW {
  static const bg      = Color(0xFF0A0A0A);
  static const surface = Color(0xFF141414);
  static const raised  = Color(0xFF1F1F1F);
  static const line    = Color(0xFF2A2A2A);
  static const txt     = Color(0xFFFFFFFF);
  static const txt2    = Color(0xFFA3A3A3);
  static const txt3    = Color(0xFF666666);
  static const gold    = Color(0xFFC9A96E);
  static const goldSoft= Color(0xFF2A2418);
  static const live    = Color(0xFFFF3B50);

  static ThemeData theme() {
    final base = ThemeData.dark(useMaterial3: true);
    return base.copyWith(
      scaffoldBackgroundColor: bg,
      colorScheme: base.colorScheme.copyWith(
        primary: gold, surface: surface,
      ),
      appBarTheme: const AppBarTheme(
        backgroundColor: bg, foregroundColor: txt, elevation: 0,
      ),
      inputDecorationTheme: InputDecorationTheme(
        filled: true, fillColor: raised,
        hintStyle: const TextStyle(color: txt3),
        border: OutlineInputBorder(
          borderRadius: BorderRadius.circular(8),
          borderSide: const BorderSide(color: line),
        ),
        enabledBorder: OutlineInputBorder(
          borderRadius: BorderRadius.circular(8),
          borderSide: const BorderSide(color: line),
        ),
        focusedBorder: OutlineInputBorder(
          borderRadius: BorderRadius.circular(8),
          borderSide: const BorderSide(color: txt2),
        ),
      ),
    );
  }

  static const mono = 'monospace';
}
