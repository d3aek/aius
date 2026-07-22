import importlib.util
import pathlib
import sys
import threading
import unittest
from http.client import HTTPConnection
from http.server import ThreadingHTTPServer


ROOT = pathlib.Path(__file__).parents[2]
TOOLS = ROOT / "tools" / "net"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location("aius_dashboard", TOOLS / "aius_dashboard.py")
DASHBOARD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DASHBOARD)


class FakeState:
    def __init__(self):
        self.lock = threading.Lock()

    def status(self):
        return {"source": "127.0.0.1", "status": {"mac": "024149550101"}}

    def request(self, *_):
        return {"type": 0x80, "payload": b""}, ("127.0.0.1", 4242)


class DashboardTests(unittest.TestCase):
    def test_loopback_routes_and_ack(self):
        server = ThreadingHTTPServer(("127.0.0.1", 0),
                                     DASHBOARD.make_handler(FakeState()))
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            connection = HTTPConnection("127.0.0.1", server.server_port)
            connection.request("GET", "/")
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertIn(b"Aius optical fixture", response.read())
            connection.request("POST", "/api/mode", b'{"mode":"flat"}',
                               {"content-type": "application/json"})
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertIn(b'"ack": true', response.read())
            connection.close()
        finally:
            server.shutdown()
            thread.join()
            server.server_close()


if __name__ == "__main__":
    unittest.main()
