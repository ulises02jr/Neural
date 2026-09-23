import 'package:flutter/material.dart';
import '../theme.dart';
import '../api.dart';
import '../widgets/brand.dart';
import 'home_screen.dart';
import 'login_screen.dart';

/// Pantalla de inicio: muestra el logo de NeuralCharts ~2s (igual que NeuralPlay)
/// y luego entra a la app (Home si hay sesión, Login si no).
class SplashScreen extends StatefulWidget {
  const SplashScreen({super.key});

  @override
  State<SplashScreen> createState() => _SplashScreenState();
}

class _SplashScreenState extends State<SplashScreen> {
  @override
  void initState() {
    super.initState();
    Future.delayed(const Duration(seconds: 2), () {
      if (!mounted) return;
      final Widget next =
          Api.I.logueado ? const HomeScreen() : const LoginScreen();
      Navigator.of(context).pushReplacement(PageRouteBuilder(
        transitionDuration: const Duration(milliseconds: 350),
        pageBuilder: (_, __, ___) => next,
        transitionsBuilder: (_, anim, __, child) =>
            FadeTransition(opacity: anim, child: child),
      ));
    });
  }

  @override
  Widget build(BuildContext context) {
    return const Scaffold(
      backgroundColor: NW.bg,
      body: Center(child: NeuralChartsWordmark(size: 140)),
    );
  }
}
