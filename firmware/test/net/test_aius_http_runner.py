import importlib.util
import json
import pathlib
import threading
import unittest
from unittest.mock import patch
from http.server import BaseHTTPRequestHandler, HTTPServer


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE = ROOT / "tools" / "net" / "aius_http_runner.py"
SPEC = importlib.util.spec_from_file_location("aius_http_runner", MODULE)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = json.dumps({"next": 7} if "events" in self.path else {"ok": True}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(length)
        body = b'{"ok":true}'
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


class HttpRunnerTests(unittest.TestCase):
    def test_request_encodes_json_and_decodes_response(self):
        server = HTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            target = ("127.0.0.1", server.server_port)
            result = RUNNER.request(target, "POST", "/api/mode", {"mode": "flat"})
            self.assertEqual(result["status"], 200)
            self.assertEqual(result["body"], {"ok": True})
        finally:
            server.shutdown()
            thread.join()
            server.server_close()

    def test_raw_response_records_clean_connection_close(self):
        result = RUNNER.raw_response_record(b"")
        self.assertEqual(result, {"method": "MALFORMED", "status": None,
                                  "outcome": "connection_closed"})

    def test_reboot_waits_for_status_after_ack(self):
        responses = [
            {"status": 200, "body": {"ok": True, "rebooting": True}},
            {"status": 200, "body": {"network_mode": "dhcp"}},
        ]
        with patch.object(RUNNER, "request", side_effect=responses) as request:
            result = RUNNER.reboot_and_wait(("127.0.0.1", 80), 1)
        self.assertEqual(result["attempts"], 1)
        self.assertEqual(request.call_count, 2)


if __name__ == "__main__":
    unittest.main()
