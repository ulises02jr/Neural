import 'package:flutter/material.dart';
import '../theme.dart';
import '../api.dart';

class RegisterScreen extends StatefulWidget {
  const RegisterScreen({super.key});

  @override
  State<RegisterScreen> createState() => _RegisterScreenState();
}

class _RegisterScreenState extends State<RegisterScreen> {
  final _codigo = TextEditingController();
  final _nombre = TextEditingController();
  final _apellido = TextEditingController();
  final _email = TextEditingController();
  final _pass = TextEditingController();
  final _pass2 = TextEditingController();
  bool _cargando = false;
  String? _error;

  Future<void> _crear() async {
    final codigo = _codigo.text.trim();
    final nombre = _nombre.text.trim();
    final apellido = _apellido.text.trim();
    final email = _email.text.trim();
    final pass = _pass.text;
    final pass2 = _pass2.text;
    if ([codigo, nombre, apellido, email, pass].any((s) => s.isEmpty)) {
      setState(() => _error = 'Completá todos los campos');
      return;
    }
    if (pass != pass2) {
      setState(() => _error = 'Las contraseñas no coinciden');
      return;
    }
    setState(() {
      _cargando = true;
      _error = null;
    });
    final r = await Api.I.unirse(
      codigo: codigo, nombre: nombre, apellido: apellido, email: email, password: pass,
    );
    if (!mounted) return;
    if (r['ok'] == true) {
      Navigator.of(context).pop();
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text(r['mensaje']?.toString() ?? 'Cuenta creada. Esperá la aprobación del administrador.'),
        duration: const Duration(seconds: 6),
      ));
    } else {
      setState(() {
        _cargando = false;
        _error = (r['mensaje'] ?? 'No se pudo crear la cuenta').toString();
      });
    }
  }

  Widget _label(String t) => Padding(
        padding: const EdgeInsets.only(bottom: 6, top: 12),
        child: Text(t,
            style: const TextStyle(
                fontSize: 11, color: NW.txt2, fontWeight: FontWeight.w500, letterSpacing: 1.2)),
      );

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Crear cuenta', style: TextStyle(fontSize: 17))),
      body: Center(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(20),
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 400),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                const Text('Unite a tu iglesia',
                    style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600)),
                const SizedBox(height: 4),
                const Text('Pedí el código de tu organización al administrador. '
                    'Tu cuenta quedará pendiente de aprobación.',
                    style: TextStyle(fontSize: 12, color: NW.txt2, height: 1.4)),
                const SizedBox(height: 8),
                if (_error != null) ...[
                  const SizedBox(height: 10),
                  Container(
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
                _label('CÓDIGO DE ORGANIZACIÓN'),
                TextField(
                  controller: _codigo,
                  autocorrect: false,
                  textCapitalization: TextCapitalization.characters,
                  decoration: const InputDecoration(hintText: 'Ej. MIISV'),
                ),
                Row(children: [
                  Expanded(child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
                    _label('NOMBRE'),
                    TextField(controller: _nombre, textCapitalization: TextCapitalization.words),
                  ])),
                  const SizedBox(width: 12),
                  Expanded(child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
                    _label('APELLIDO'),
                    TextField(controller: _apellido, textCapitalization: TextCapitalization.words),
                  ])),
                ]),
                _label('EMAIL'),
                TextField(
                  controller: _email,
                  keyboardType: TextInputType.emailAddress,
                  autocorrect: false,
                ),
                _label('CONTRASEÑA'),
                TextField(controller: _pass, obscureText: true),
                _label('REPETIR CONTRASEÑA'),
                TextField(controller: _pass2, obscureText: true, onSubmitted: (_) => _crear()),
                const SizedBox(height: 22),
                FilledButton(
                  style: FilledButton.styleFrom(
                    backgroundColor: Colors.white,
                    foregroundColor: Colors.black,
                    padding: const EdgeInsets.symmetric(vertical: 14),
                    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
                  ),
                  onPressed: _cargando ? null : _crear,
                  child: _cargando
                      ? const SizedBox(
                          height: 20, width: 20,
                          child: CircularProgressIndicator(strokeWidth: 2, color: Colors.black))
                      : const Text('Crear cuenta',
                          style: TextStyle(fontWeight: FontWeight.w600, fontSize: 14)),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
