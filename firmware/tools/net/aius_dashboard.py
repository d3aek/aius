#!/usr/bin/env python3
"""Loopback-only Aius optical fixture dashboard.

The browser never talks to the Teensy directly. This service is a deliberately
small control-plane proxy over the versioned UDP runner.
"""

import argparse
import json
import struct
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import aius_net_runner as net


PAGE = """<!doctype html>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Aius optical fixture</title>
<style>
:root { color-scheme: dark; font: 16px system-ui, sans-serif; }
body { max-width: 900px; margin: 2rem auto; padding: 0 1rem; background:#111827; color:#e5e7eb; }
main { display:grid; gap:1rem; }
section { background:#1f2937; border:1px solid #374151; border-radius:10px; padding:1rem; }
button { margin:.25rem; padding:.55rem .8rem; border:0; border-radius:6px; cursor:pointer; }
button[data-mode] { background:#60a5fa; color:#07111f; }
button[data-test] { background:#fbbf24; color:#221500; }
code, pre { white-space:pre-wrap; word-break:break-word; }
.offline { color:#fca5a5; } .ok { color:#86efac; }
</style>
<main>
<h1>Aius optical fixture</h1>
<section><div id="identity">Connecting…</div><pre id="status"></pre></section>
<section><strong>DSP preset</strong><div>
<button data-mode="flat">Flat</button><button data-mode="speech">Speech</button>
<button data-mode="bass-reduced">Bass reduced</button></div>
<strong>Mix</strong><div>
<button data-mix="standard">Standard</button><button data-mix="lfe-minus10">LFE −10 dB</button>
<button data-mix="clipfree">Clip-free</button></div>
<strong>Observation</strong><div>
<button data-test="start">Start test</button><button data-test="stop">Stop test</button>
<button id="clear">Clear counters</button></div></section>
<section><strong>Events</strong><pre id="events"></pre></section>
</main>
<script>
const status = document.querySelector('#status');
const identity = document.querySelector('#identity');
const events = document.querySelector('#events');
let cursor = 0;
async function call(path, options) {
  const response = await fetch(path, options);
  const body = await response.json();
  if (!response.ok) throw new Error(body.error || 'request failed');
  return body;
}
async function refresh() {
  try {
    const body = await call('/api/status');
    const s = body.status;
    identity.className = 'ok';
    identity.textContent = `${s.mac} · ${s.ip} · boot ${s.boot_epoch}`;
    status.textContent = JSON.stringify(s, null, 2);
    const e = await call(`/api/events?cursor=${cursor}`);
    if (e.events.length) {
      cursor = Math.max(...e.events.map(x => x.sequence));
      events.textContent = `${events.textContent}\n${e.events.map(x => JSON.stringify(x)).join('\n')}`.trim();
    }
  } catch (error) {
    identity.className = 'offline';
    identity.textContent = `Offline: ${error}`;
  }
}
async function command(path, body) {
  try { await call(path, {method:'POST', headers:{'content-type':'application/json'}, body:JSON.stringify(body || {})}); await refresh(); }
  catch (error) { identity.className='offline'; identity.textContent=`Command failed: ${error}`; }
}
document.querySelectorAll('[data-mode]').forEach(x => x.onclick=() => command('/api/mode', {mode:x.dataset.mode}));
document.querySelectorAll('[data-mix]').forEach(x => x.onclick=() => command('/api/mix', {mix:x.dataset.mix}));
document.querySelectorAll('[data-test]').forEach(x => x.onclick=() => command('/api/test', {action:x.dataset.test}));
document.querySelector('#clear').onclick = () => command('/api/clear', {});
refresh(); setInterval(refresh, 1000);
</script>
"""


class DashboardState:
    def __init__(self, target, expected_mac=None, timeout=0.25):
        self.target = (target, net.PORT)
        self.expected_mac = expected_mac.lower() if expected_mac else None
        self.timeout = timeout
        self.lock = threading.Lock()

    def request(self, msg_type, payload=b""):
        response, address = net.request(self.target, msg_type, payload,
                                         timeout=self.timeout)
        if response["type"] == 0x81:
            error = response["payload"][0] if response["payload"] else "unknown"
            raise ValueError(f"device NACK: {error}")
        return response, address

    def status(self):
        response, address = self.request(net.CMD_GET_STATUS)
        status = net.parse_status(response["payload"])
        if self.expected_mac and status["mac"].lower() != self.expected_mac:
            raise ValueError("device MAC does not match configured target")
        return {"source": address[0], "status": status}


def json_body(handler):
    length = int(handler.headers.get("content-length", "0"))
    if length > 256:
        raise ValueError("request body too large")
    return json.loads(handler.rfile.read(length) or b"{}")


def make_handler(state):
    class Handler(BaseHTTPRequestHandler):
        def send_json(self, code, value):
            data = json.dumps(value).encode()
            self.send_response(code)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            parsed = urlparse(self.path)
            try:
                if parsed.path == "/":
                    data = PAGE.encode()
                    self.send_response(200)
                    self.send_header("content-type", "text/html; charset=utf-8")
                    self.send_header("content-length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                elif parsed.path == "/api/status":
                    with state.lock:
                        self.send_json(200, state.status())
                elif parsed.path == "/api/events":
                    cursor = int(parse_qs(parsed.query).get("cursor", ["0"])[0])
                    response, _ = state.request(net.CMD_GET_EVENTS,
                                                struct.pack("<I", cursor))
                    self.send_json(200, {"events": net.parse_events(response["payload"])})
                else:
                    self.send_json(404, {"error": "not found"})
            except (OSError, TimeoutError, ValueError, json.JSONDecodeError) as error:
                self.send_json(502, {"error": str(error)})

        def do_POST(self):
            try:
                body = json_body(self)
                if self.path == "/api/mode":
                    modes = {"flat": 0, "speech": 1, "bass-reduced": 2}
                    payload = bytes((modes[body["mode"]],))
                    command = net.CMD_SET_MODE
                elif self.path == "/api/mix":
                    mixes = {"standard": 0, "lfe-minus10": 1, "clipfree": 2}
                    payload = bytes((mixes[body["mix"]],))
                    command = net.CMD_SET_MIX
                elif self.path == "/api/test":
                    command = {"start": net.CMD_START_TEST,
                               "stop": net.CMD_STOP_TEST}[body["action"]]
                    payload = b""
                elif self.path == "/api/clear":
                    command, payload = net.CMD_CLEAR_COUNTERS, b""
                else:
                    self.send_json(404, {"error": "not found"})
                    return
                with state.lock:
                    response, _ = state.request(command, payload)
                if response["type"] != net.RSP_ACK:
                    raise ValueError("device did not acknowledge command")
                self.send_json(200, {"ack": True})
            except (KeyError, OSError, TimeoutError, ValueError, json.JSONDecodeError) as error:
                self.send_json(400, {"error": str(error)})

        def log_message(self, *_):
            return

    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", required=True)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--expected-mac")
    args = parser.parse_args()
    state = DashboardState(args.target, args.expected_mac)
    server = ThreadingHTTPServer((args.bind, args.port), make_handler(state))
    print(f"Aius dashboard listening on http://{args.bind}:{args.port}/")
    server.serve_forever()


if __name__ == "__main__":
    main()
