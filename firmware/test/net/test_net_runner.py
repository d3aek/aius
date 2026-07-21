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


if __name__ == "__main__":
    unittest.main()
