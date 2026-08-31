import 'package:flutter_test/flutter_test.dart';
import 'package:wings_app/main.dart';

void main() {
  testWidgets('Wings app smoke', (WidgetTester tester) async {
    await tester.pumpWidget(const WingsApp());
    expect(find.text('Wings'), findsOneWidget);
  });
}
