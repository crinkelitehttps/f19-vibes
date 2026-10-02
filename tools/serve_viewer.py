"""Static server for the viewer, plus a POST sink for its self-test.

Usage: python3 tools/serve_viewer.py [PORT]   then open http://localhost:PORT/viewer/
Self-test: open /viewer/?selftest; screenshots and report.json land in out/viewer_report/.
"""
import base64
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPORT = ROOT / "out/viewer_report"


class Handler(SimpleHTTPRequestHandler):
    def do_POST(self):
        if not self.path.startswith("/report/"):
            self.send_error(404)
            return
        name = Path(self.path[len("/report/"):]).name
        body = self.rfile.read(int(self.headers["Content-Length"]))
        if name.endswith(".png") and body.startswith(b"data:image/png;base64,"):
            body = base64.b64decode(body.split(b",", 1)[1])
        REPORT.mkdir(parents=True, exist_ok=True)
        (REPORT / name).write_bytes(body)
        self.send_response(204)
        self.end_headers()

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8019
    print(f"http://localhost:{port}/viewer/")
    ThreadingHTTPServer(("127.0.0.1", port), partial(Handler, directory=str(ROOT))).serve_forever()
