import 'package:flutter_test/flutter_test.dart';
import 'package:wings_app/protocol.dart';

void main() {
  test('status blob v2 without pair fields stays NONE', () {
    final b = List<int>.filled(36, 0);
    b[1] = 1; // master
    b[26] = 10;
    b[31] = 100;
    b[32] = 100;
    b[33] = 100;
    b[34] = 100;
    b[35] = 100;
    final s = WingsStatus.fromBytes(b);
    expect(s.isMaster, isTrue);
    expect(s.slaveLink, SlaveLink.none);
    expect(macIsZero(s.peerMac), isTrue);
    expect(s.canLinkSlave, isTrue);
  });

  test('status blob with pair fields', () {
    final b = List<int>.filled(50, 0);
    b[1] = 1;
    b[26] = 25;
    for (var i = 0; i < 5; i++) {
      b[31 + i] = 100;
    }
    b[36] = SlaveLink.heard;
    b[37] = 3;
    b[38] = 0xd4;
    b[43] = 0xc4;
    b[44] = 0xaa;
    b[49] = 0xff;
    final s = WingsStatus.fromBytes(b);
    expect(s.slaveLink, SlaveLink.heard);
    expect(s.canLinkSlave, isTrue);
    expect(s.canUnlinkSlave, isFalse);
    expect(s.slaveAgeCs, 3);
    expect(formatMac(s.localMac), 'd4:00:00:00:00:c4');
    expect(formatMac(s.peerMac), 'aa:00:00:00:00:ff');
  });

  test('NONE with last-heard MAC can LINK', () {
    final b = List<int>.filled(50, 0);
    b[1] = 1;
    b[26] = 25;
    for (var i = 0; i < 5; i++) {
      b[31 + i] = 100;
    }
    b[36] = SlaveLink.none;
    b[44] = 0xd4;
    b[45] = 0x05;
    b[46] = 0x92;
    b[47] = 0x40;
    b[48] = 0xc2;
    b[49] = 0xd0;
    final s = WingsStatus.fromBytes(b);
    expect(s.canLinkSlave, isTrue);
    expect(s.canUnlinkSlave, isFalse);
    expect(formatMac(s.peerMac), 'd4:05:92:40:c2:d0');
  });
}
