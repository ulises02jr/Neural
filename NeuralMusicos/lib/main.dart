import 'package:flutter/material.dart';
import 'theme.dart';
import 'api.dart';
import 'app_channel.dart';
import 'screens/splash_screen.dart';
import 'screens/login_screen.dart';

/// Navigator global para poder navegar/avisar desde fuera del árbol de widgets
/// (por ejemplo, cuando el servidor cierra la sesión por otro dispositivo).
final GlobalKey<NavigatorState> navKey = GlobalKey<NavigatorState>();

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await AppChannel.I.load();
  await Api.I.cargarSesion();

  // Sesión única: si el servidor expulsa esta sesión (se abrió en otro
  // dispositivo), avisamos y volvemos al login.
  Api.I.onKicked = (String mensaje) {
    final ctx = navKey.currentContext;
    if (ctx == null) return;
    showDialog(
      context: ctx,
      barrierDismissible: false,
      builder: (d) => AlertDialog(
        backgroundColor: NW.surface,
        title: const Text('Sesión cerrada', style: TextStyle(color: NW.txt)),
        content: Text(mensaje, style: const TextStyle(color: NW.txt2)),
        actions: [
          TextButton(
            onPressed: () {
              Navigator.of(d).pop();
              navKey.currentState?.pushAndRemoveUntil(
                MaterialPageRoute(builder: (_) => const LoginScreen()),
                (route) => false,
              );
            },
            child: const Text('Entendido'),
          ),
        ],
      ),
    );
  };

  runApp(const NeuralMusicosApp());
}

class NeuralMusicosApp extends StatelessWidget {
  const NeuralMusicosApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'NeuralCharts',
      debugShowCheckedModeBanner: false,
      navigatorKey: navKey,
      theme: NW.theme(),
      home: const SplashScreen(),
    );
  }
}
