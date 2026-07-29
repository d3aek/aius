import importlib.util
import pathlib
import struct
import unittest


ROOT = pathlib.Path(__file__).parents[2]
MODULE = ROOT / "tools" / "net" / "aius_net_runner.py"
FIXTURES = ROOT / "test" / "fixtures" / "status"
LEGACY_STATUS = (FIXTURES / "status-v1-71.bin").read_bytes()
RECOVERY_STATUS = (FIXTURES / "status-v1-recovery-99.bin").read_bytes()
SPEC = importlib.util.spec_from_file_location("aius_net_runner", MODULE)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)

EXPECTED_LEGACY_STATUS = {
    "network_mode": "dhcp",
    "state": "dts-active",
    "carrier_locked": True,
    "boot_epoch": 0x01020304,
    "uptime_ms": 0x11121314,
    "ip": "192.0.2.45",
    "mac": "02:41:49:55:a5:5a",
    "ntp_valid": True,
    "format": "dts",
    "pipeline_status": 127,
    "sample_rate_hz": 48000,
    "pipeline_blocks": 0x21222324,
    "pipeline_overruns": 0x31323334,
    "test_running": True,
    "eq_mode": "bass-reduced",
    "mix_mode": "lfe-minus10",
    "event_sequence": 0x41424344,
    "event_overflows": 0x51525354,
    "network_drops": 0x61626364,
    "network_budget_overruns": 0x71727374,
    "http_requests": 0x81828384,
    "http_rejections": 0x91929394,
    "http_client_drops": 0xa1a2a3a4,
    "http_service_overruns": 0xb1b2b3b4,
}

EXPECTED_RECOVERY_STATUS = {
    **EXPECTED_LEGACY_STATUS,
    "network_state": 5,
    "recovery_safe": True,
    "network_last_fault": 10,
    "last_checkpoint": 10,
    "network_generation": 0xc1c2c3c4,
    "boot_generation": 0xd1d2d3d4,
    "reset_source": 0xe1e2e3e4,
    "watchdog_early_resets": 254,
    "loop_epoch": 0xf1f2f3f4,
    "network_restarts": 0x01020305,
}


class RunnerProtocolTests(unittest.TestCase):
    def test_round_trip_and_status(self):
        frame = RUNNER.encode(RUNNER.CMD_GET_STATUS, 0x12345678, b"abc")
        decoded = RUNNER.decode(frame)
        self.assertEqual(decoded["request_id"], 0x12345678)
        self.assertEqual(decoded["payload"], b"abc")

        payload = bytearray(71)
        payload[0] = 1
        payload[1] = 1
        payload[2] = 2
        payload[3] = 1
        struct.pack_into("<II", payload, 4, 7, 1000)
        payload[12:16] = bytes((192, 168, 42, 10))
        payload[16:22] = bytes.fromhex("024149550101")
        status = RUNNER.parse_status(payload)
        self.assertEqual(status["network_mode"], "dhcp")
        self.assertTrue(status["carrier_locked"])

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
        payload = bytearray(71)
        payload[0] = 1
        payload[3] = 2
        payload[37] = 1
        payload[38] = 2
        status = RUNNER.parse_status(payload)
        self.assertEqual(status["eq_mode"], "speech")
        self.assertTrue(status["test_running"])
        self.assertEqual(status["mix_mode"], "clipfree")

    def test_extended_status_network_and_http_counters(self):
        payload = bytearray(71)
        payload[0] = 1
        struct.pack_into("<IIIIIIII", payload, 39, 10, 2, 3, 4, 5, 6, 7, 8)
        status = RUNNER.parse_status(payload)
        self.assertEqual(status["event_sequence"], 10)
        self.assertEqual(status["network_budget_overruns"], 4)
        self.assertEqual(status["http_requests"], 5)
        self.assertEqual(status["http_service_overruns"], 8)

    def test_precommitted_legacy_fixture_has_exact_mapping(self):
        self.assertEqual(len(LEGACY_STATUS), 71)
        status = RUNNER.parse_status(LEGACY_STATUS)
        self.assertEqual(status, EXPECTED_LEGACY_STATUS)
        bool_fields = {"carrier_locked", "ntp_valid", "test_running"}
        str_fields = {
            "network_mode",
            "state",
            "ip",
            "mac",
            "format",
            "eq_mode",
            "mix_mode",
        }
        for field in EXPECTED_LEGACY_STATUS:
            expected_type = bool if field in bool_fields else (
                str if field in str_fields else int)
            with self.subTest(field=field):
                self.assertIs(type(status[field]), expected_type)

    def test_precommitted_recovery_fixture_has_exact_mapping_and_types(self):
        self.assertEqual(len(RECOVERY_STATUS), 99)
        self.assertEqual(RECOVERY_STATUS[:71], LEGACY_STATUS)
        status = RUNNER.parse_status(RECOVERY_STATUS)
        self.assertEqual(status, EXPECTED_RECOVERY_STATUS)
        integer_fields = (
            "network_state",
            "network_generation",
            "network_last_fault",
            "boot_generation",
            "reset_source",
            "watchdog_early_resets",
            "last_checkpoint",
            "loop_epoch",
            "network_restarts",
        )
        for field in integer_fields:
            with self.subTest(field=field):
                self.assertIs(type(status[field]), int)
        self.assertIs(type(status["recovery_safe"]), bool)

    def test_legacy_static_mode_is_deliberately_decodable(self):
        payload = bytearray(LEGACY_STATUS)
        payload[1] = 2
        self.assertEqual(RUNNER.parse_status(payload)["network_mode"],
                         "legacy-static")

    def test_every_other_protocol_payload_length_rejects(self):
        for length in list(range(RUNNER.MAX_PAYLOAD + 1)) + [65535]:
            if length in (71, 99):
                continue
            payload = (RECOVERY_STATUS + bytes(length))[:length]
            with self.subTest(length=length):
                with self.assertRaises(ValueError):
                    RUNNER.parse_status(payload)

    def test_every_unsupported_version_rejects_for_both_schemas(self):
        for length, fixture in ((71, LEGACY_STATUS), (99, RECOVERY_STATUS)):
            for version in range(256):
                if version == 1:
                    continue
                payload = bytearray(fixture)
                payload[0] = version
                with self.subTest(length=length, version=version):
                    with self.assertRaises(ValueError):
                        RUNNER.parse_status(payload)

    def test_enumerated_invalid_recovery_domains_reject(self):
        invalid_values = {
            71: (6, 255),
            72: (2, 255),
            73: (11, 255),
            74: (11, 255),
        }
        for offset, values in invalid_values.items():
            for value in values:
                payload = bytearray(RECOVERY_STATUS)
                payload[offset] = value
                with self.subTest(offset=offset, value=value):
                    with self.assertRaises(ValueError):
                        RUNNER.parse_status(payload)

    def test_each_reserved_byte_rejects_every_nonzero_value_independently(self):
        for offset in range(88, 91):
            for value in range(1, 256):
                payload = bytearray(RECOVERY_STATUS)
                payload[offset] = value
                with self.subTest(offset=offset, value=value):
                    with self.assertRaises(ValueError):
                        RUNNER.parse_status(payload)

    def test_recovery_domain_endpoints_parse(self):
        payload = bytearray(RECOVERY_STATUS)
        payload[71] = 0
        payload[72] = 0
        payload[73] = 0
        payload[74] = 0
        status = RUNNER.parse_status(payload)
        self.assertEqual(status["network_state"], 0)
        self.assertIs(status["recovery_safe"], False)
        self.assertEqual(status["network_last_fault"], 0)
        self.assertEqual(status["last_checkpoint"], 0)

        status = RUNNER.parse_status(RECOVERY_STATUS)
        self.assertEqual(status["network_state"], 5)
        self.assertIs(status["recovery_safe"], True)
        self.assertEqual(status["network_last_fault"], 10)
        self.assertEqual(status["last_checkpoint"], 10)

    def test_recovery_extension_leaves_the_legacy_decode_identical(self):
        # The device produces the 99-byte payload by appending to the same 71
        # bytes it used to send, so every legacy field must decode to the same
        # value from either shape.
        legacy = RUNNER.parse_status(RECOVERY_STATUS[:71])
        recovery = RUNNER.parse_status(RECOVERY_STATUS)
        for field, value in legacy.items():
            with self.subTest(field=field):
                self.assertEqual(recovery[field], value)
        self.assertEqual(set(recovery) - set(legacy), set(EXPECTED_RECOVERY_STATUS) - set(legacy))

    def test_recovery_shape_parses_the_network_mode_byte_the_device_sends(self):
        # put_status() leaves byte 1 at 0: the mode producer was retired with
        # the legacy network path, and the firmware encoder accepts 0 and 1.
        for mode, expected in ((0, "unknown"), (1, "dhcp")):
            payload = bytearray(RECOVERY_STATUS)
            payload[1] = mode
            with self.subTest(mode=mode):
                status = RUNNER.parse_status(payload)
                self.assertEqual(status["network_mode"], expected)
                self.assertEqual(status["recovery_safe"], True)
                self.assertEqual(status["last_checkpoint"], 10)

    def test_recovery_integer_fields_do_not_narrow(self):
        payload = bytearray(RECOVERY_STATUS)
        for offset in (75, 79, 83, 91, 95):
            payload[offset:offset + 4] = b"\xff\xff\xff\xff"
        payload[87] = 255
        status = RUNNER.parse_status(payload)
        for field in (
                "network_generation",
                "boot_generation",
                "reset_source",
                "loop_epoch",
                "network_restarts"):
            with self.subTest(field=field):
                self.assertEqual(status[field], 0xffffffff)
        self.assertEqual(status["watchdog_early_resets"], 255)


if __name__ == "__main__":
    unittest.main()
