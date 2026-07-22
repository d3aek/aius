import importlib.util
import pathlib
import struct
import unittest


ROOT = pathlib.Path(__file__).parents[2]
MODULE = ROOT / "tools" / "net" / "aius_net_runner.py"
SPEC = importlib.util.spec_from_file_location("aius_net_runner", MODULE)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class RunnerProtocolTests(unittest.TestCase):
    def test_round_trip_and_status(self):
        frame = RUNNER.encode(RUNNER.CMD_GET_STATUS, 0x12345678, b"abc")
        decoded = RUNNER.decode(frame)
        self.assertEqual(decoded["request_id"], 0x12345678)
        self.assertEqual(decoded["payload"], b"abc")

        payload = bytearray(23)
        payload[0] = 1
        payload[1] = 1
        payload[2] = 2
        payload[3] = 1
        struct.pack_into("<II", payload, 4, 7, 1000)
        payload[12:16] = bytes((192, 168, 42, 10))
        payload[16:22] = bytes.fromhex("024149550101")
        self.assertEqual(RUNNER.parse_status(payload)["network_mode"], "dhcp")
        self.assertTrue(RUNNER.parse_status(payload)["carrier_locked"])

    def test_corruption_rejected(self):
        frame = bytearray(RUNNER.encode(RUNNER.CMD_DISCOVER, 1))
        frame[-1] ^= 0x01
        with self.assertRaises(ValueError):
            RUNNER.decode(frame)

    def test_events_cursor_payload(self):
        payload = bytearray(13)
        payload[0] = 1
        struct.pack_into("<II", payload, 1, 3, 900)
        payload[9] = 1
        payload[10] = 3
        struct.pack_into("<H", payload, 11, 0)
        events = RUNNER.parse_events(payload)
        self.assertEqual(events[0]["sequence"], 3)
        self.assertEqual(events[0]["state"], 3)
        with self.assertRaises(ValueError):
            RUNNER.parse_events(payload[:-1])

    def test_extended_status_mode(self):
        payload = bytearray(38)
        payload[0] = 1
        payload[3] = 2
        payload[37] = 1
        status = RUNNER.parse_status(payload)
        self.assertEqual(status["eq_mode"], "speech")
        self.assertTrue(status["test_running"])


if __name__ == "__main__":
    unittest.main()
