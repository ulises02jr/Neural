import 'package:flutter/material.dart';
import '../theme.dart';
import '../api.dart';
import 'home_screen.dart';
import 'register_screen.dart';

class LoginScreen extends StatefulWidget {
  const LoginScreen({super.key});

  @override
  State<LoginScreen> createState() => _LoginScreenState();
}

class _LoginScreenState extends State<LoginScreen> {
  final _email = TextEditingController();
  final _pass = TextEditingController();
  bool _cargando = false;
  String? _error;

  Future<void> _entrar() async {
    final email = _email.text.trim();
    final pass = _pass.text;
    if (email.isEmpty || pass.isEmpty) {
      setState(() => _error = 'Escribi tu email y contrasena');
      return;
    }
    setState(() {
      _cargando = true;
      _error = null;
    });
    final r = await Api.I.login(email, pass);
    if (!mounted) return;
    if (r['ok'] == true) {
      Navigator.of(context).pushReplacement(
        MaterialPageRoute(builder: (_) => const HomeScreen()),
      );
    } else {
      setState(() {
        _cargando = false;
        _error = (r['mensaje'] ?? 'No se pudo iniciar sesion').toString();
      });
    }
  }

  Widget _label(String t) => Padding(
        padding: const EdgeInsets.only(bottom: 6),
        child: Text(t,
            style: const TextStyle(
                fontSize: 11,
                color: NW.txt2,
                fontWeight: FontWeight.w500,
                letterSpacing: 1.2)),
      );

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Center(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(20),
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 380),
            child: Container(
              padding: const EdgeInsets.fromLTRB(28, 36, 28, 28),
              decoration: BoxDecoration(
                color: NW.surface,
                border: Border.all(color: NW.line),
                borderRadius: BorderRadius.circular(14),
              ),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  // Logo circular limpio (recorte a circulo, llena el area).
                  Center(
                    child: ClipOval(
                      child: Container(
                        width: 90,
                        height: 90,
                        color: NW.raised,
                        child: Image.network(
                          '${Api.baseUrl}/static/logo.png',
                          fit: BoxFit.cover,
                          errorBuilder: (_, __, ___) =>
                              const Icon(Icons.graphic_eq, color: NW.chord, size: 40),
                        ),
                      ),
                    ),
                  ),
                  const SizedBox(height: 18),
                  const Text('Neural Worship',
                      textAlign: TextAlign.center,
                      style: TextStyle(
                          fontSize: 18, fontWeight: FontWeight.w600, letterSpacing: 0.3)),
                  const SizedBox(height: 4),
                  const Text('Acceso de musicos',
                      textAlign: TextAlign.center,
                      style: TextStyle(fontSize: 13, color: NW.txt2)),
                  const SizedBox(height: 28),
                  if (_error != null) ...[
                    Container(
                      margin: const EdgeInsets.only(bottom: 16),
                      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 11),
                      decoration: BoxDecoration(
                        color: const Color(0x1FDC6262),
                        borderRadius: BorderRadius.circular(8),
                        border: Border.all(color: const Color(0x40DC6262)),
                      ),
                      child: Text(_error!,
                          textAlign: TextAlign.center,
                          style: const TextStyle(color: NW.error, fontSize: 13)),
                    ),
                  ],
                  _label('EMAIL'),
                  TextField(
                    controller: _email,
                    keyboardType: TextInputType.emailAddress,
                    autocorrect: false,
                    textInputAction: TextInputAction.next,
                  ),
                  const SizedBox(height: 14),
                  _label('CONTRASEÑA'),
                  TextField(
                    controller: _pass,
                    obscureText: true,
                    onSubmitted: (_) => _entrar(),
                  ),
                  const SizedBox(height: 20),
                  FilledButton(
                    style: FilledButton.styleFrom(
                      backgroundColor: Colors.white,
                      foregroundColor: Colors.black,
                      padding: const EdgeInsets.symmetric(vertical: 14),
                      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
                    ),
                    onPressed: _cargando ? null : _entrar,
                    child: _cargando
                        ? const SizedBox(
                            height: 20,
                            width: 20,
                            child: CircularProgressIndicator(strokeWidth: 2, color: Colors.black))
                        : const Text('Ingresar',
                            style: TextStyle(
                                fontWeight: FontWeight.w600, fontSize: 14, letterSpacing: 0.3)),
                  ),
                  const SizedBox(height: 18),
                  Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      const Text('¿No tenés cuenta? ',
                          style: TextStyle(fontSize: 12, color: NW.txt3)),
                      GestureDetector(
                        onTap: () => Navigator.of(context).push(
                          MaterialPageRoute(builder: (_) => const RegisterScreen()),
                        ),
                        child: const Text('Crear una',
                            style: TextStyle(
                                fontSize: 12, color: NW.chord, fontWeight: FontWeight.w600)),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}
