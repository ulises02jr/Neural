import 'package:flutter/material.dart';
import '../theme.dart';

/// Glifo de NeuralCharts (≡ ·— · ≡): dos grupos de 3 barras conectados por un nodo.
/// Dibujado como vector para verse nítido en cualquier tamaño.
class _GlyphPainter extends CustomPainter {
  final Color color;
  _GlyphPainter(this.color);

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width;                 // usa TODO el ancho → glifo centrado
    final cy = size.height / 2;           // centrado vertical
    final p = Paint()
      ..color = color
      ..style = PaintingStyle.fill
      ..isAntiAlias = true;
    final barH = w * 0.115;
    final r = barH / 2;
    final gap = w * 0.16;                 // separación vertical entre filas
    final y0 = cy - gap, y1 = cy, y2 = cy + gap;
    void bar(double x, double yc, double bw) => canvas.drawRRect(
        RRect.fromRectAndRadius(
            Rect.fromLTWH(x, yc - barH / 2, bw, barH), Radius.circular(r)),
        p);

    // grupo izquierdo (alineado a la izquierda): arriba/medio largas, abajo corta
    final lx = w * 0.11;
    bar(lx, y0, w * 0.30);
    bar(lx, y1, w * 0.30);
    bar(lx, y2, w * 0.22);
    // grupo derecho (alineado a la derecha), espejo
    final rEnd = w * 0.89;
    bar(rEnd - w * 0.30, y0, w * 0.30);
    bar(rEnd - w * 0.30, y1, w * 0.30);
    bar(rEnd - w * 0.22, y2, w * 0.22);
    // conector central: punto — línea — punto (dead center)
    final line = Paint()
      ..color = color
      ..strokeWidth = w * 0.026
      ..strokeCap = StrokeCap.round
      ..isAntiAlias = true;
    final cx1 = w * 0.455, cx2 = w * 0.545, dotR = w * 0.030;
    canvas.drawLine(Offset(cx1, y1), Offset(cx2, y1), line);
    canvas.drawCircle(Offset(cx1, y1), dotR, p);
    canvas.drawCircle(Offset(cx2, y1), dotR, p);
  }

  @override
  bool shouldRepaint(covariant _GlyphPainter old) => old.color != color;
}

/// Solo el glifo (cuadrado). Ideal para la esquina del header.
class NeuralChartsIcon extends StatelessWidget {
  final double size;
  final Color color;
  const NeuralChartsIcon({super.key, this.size = 28, this.color = NW.txt});
  @override
  Widget build(BuildContext context) => SizedBox(
      width: size,
      height: size,
      child: CustomPaint(painter: _GlyphPainter(color)));
}

/// Logo completo: glifo arriba + "Neural" (regular) / "Charts" (bold) en Space Grotesk.
/// Es el wordmark que se muestra en el splash de inicio.
class NeuralChartsWordmark extends StatelessWidget {
  final double size;
  const NeuralChartsWordmark({super.key, this.size = 132});
  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        SizedBox(
            width: size,
            height: size * 0.50,
            child: CustomPaint(painter: _GlyphPainter(NW.txt))),
        SizedBox(height: size * 0.16),
        Text('Neural',
            style: TextStyle(
                fontFamily: NW.brand,
                fontWeight: FontWeight.w400,
                fontSize: size * 0.30,
                height: 1.0,
                color: NW.txt)),
        Text('Charts',
            style: TextStyle(
                fontFamily: NW.brand,
                fontWeight: FontWeight.w700,
                fontSize: size * 0.30,
                height: 1.05,
                color: NW.txt)),
      ],
    );
  }
}
