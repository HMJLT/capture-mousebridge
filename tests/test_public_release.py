import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).parents[1]


def mouse_packet(dx: int, dy: int, buttons: int, wheel: int, sequence: int) -> bytes:
    packet = bytearray(16)
    packet[0] = 0x02
    struct.pack_into("<h", packet, 8, dx)
    struct.pack_into("<h", packet, 10, dy)
    packet[12] = buttons & 0x07
    packet[13] = wheel & 0xFF
    struct.pack_into("<H", packet, 14, sequence & 0xFFFF)
    return bytes(packet)


class PublicReleaseTests(unittest.TestCase):
    def test_udp_mouse_packet_is_sixteen_bytes_and_little_endian(self):
        packet = mouse_packet(-12, 37, 3, -1, 0x1234)
        self.assertEqual(len(packet), 16)
        self.assertEqual(packet[0], 0x02)
        self.assertEqual(struct.unpack_from("<h", packet, 8)[0], -12)
        self.assertEqual(struct.unpack_from("<h", packet, 10)[0], 37)
        self.assertEqual(packet[12:14], bytes((3, 255)))
        self.assertEqual(struct.unpack_from("<H", packet, 14)[0], 0x1234)

    def test_public_docs_describe_hotspot_and_mcp(self):
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn("ESP32 固件与热点", readme)
        self.assertIn("MCP 接口说明", readme)
        self.assertTrue((ROOT / "docs" / "MCP.md").is_file())
        self.assertTrue((ROOT / "docs" / "ESP32-WIFI.md").is_file())
        self.assertTrue((ROOT / "docs" / "wiki" / "Architecture.md").is_file())


if __name__ == "__main__":
    unittest.main()
