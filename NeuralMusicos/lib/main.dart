import 'package:flutter/material.dart';
import 'theme.dart';
import 'api.dart';
import 'app_channel.dart';
import 'screens/login_screen.dart';
import 'screens/home_screen.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await AppChannel.I.load();
  await Api.I.cargarSesion();
  runApp(const NeuralMusicosApp());
}

class NeuralMusicosApp extends StatelessWidget {
  const NeuralMusicosApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Neural Worship',
      debugShowCheckedModeBanner: false,
      theme: NW.theme(),
      home: Api.I.logueado ? const HomeScreen() : const LoginScreen(),
    );
  }
}
