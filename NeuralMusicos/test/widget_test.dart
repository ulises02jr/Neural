import 'package:flutter_test/flutter_test.dart';
import 'package:neural_musicos/main.dart';

void main() {
  testWidgets('La app arranca en la pantalla de login', (WidgetTester tester) async {
    await tester.pumpWidget(const NeuralMusicosApp());
    await tester.pump();
    expect(find.text('Neural Worship'), findsWidgets);
    expect(find.text('Entrar'), findsOneWidget);
  });
}
