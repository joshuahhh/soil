#!/usr/bin/env python3
"""Dev server for web/: http.server plus Cache-Control: no-store, so phones
and test browsers never run stale copies of earth.js/wasm."""
import http.server
import os
import socket

class NoStoreHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

if __name__ == "__main__":
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "web"))
    print("serving on http://localhost:8000")
    try:
        # the LAN address, for phones; a throwaway UDP "connection" (nothing
        # is sent) is the portable way to learn which interface routes out
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        print(f"        and http://{s.getsockname()[0]}:8000")
        s.close()
    except OSError:
        pass
    http.server.ThreadingHTTPServer(("", 8000), NoStoreHandler).serve_forever()
