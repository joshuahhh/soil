#!/usr/bin/env python3
"""Dev server for web/: http.server plus Cache-Control: no-store, so phones
and test browsers never run stale copies of earth.js/wasm."""
import http.server
import functools
import os

class NoStoreHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    http.server.ThreadingHTTPServer(("", 8000), NoStoreHandler).serve_forever()
