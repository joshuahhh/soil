#!/usr/bin/env python3
"""Dev server for web/: http.server plus Cache-Control: no-store, so phones
and test browsers never run stale copies of earth.js/wasm."""
import http.server
import json
import re
import os
import socket
import subprocess

ROOT = os.path.dirname(os.path.abspath(__file__))

def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True,
                          text=True, check=True).stdout.strip()

# the ? menu's version section. ci writes a static one for pages (deploy.yml);
# here it's live, so it shows uncommitted edits as they happen
def version():
    out = {"commit": git("rev-parse", "HEAD"), "dirty": git("status", "--porcelain") != ""}
    try:
        m = re.search(r"github\.com[:/](.+?)(\.git)?$", git("remote", "get-url", "origin"))
        if m:
            out["repo"] = m.group(1)
    except subprocess.CalledProcessError:
        pass
    return out

class NoStoreHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.split("?")[0] != "/version.json":
            return super().do_GET()
        try:
            body = json.dumps(version()).encode()
        except (OSError, subprocess.CalledProcessError):
            return self.send_error(404)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

if __name__ == "__main__":
    os.chdir(os.path.join(ROOT, "web"))
    # 8000, or the next free port after it (as vite does), so a second
    # checkout's server doesn't just die
    for port in range(8000, 8100):
        try:
            server = http.server.ThreadingHTTPServer(("", port), NoStoreHandler)
            break
        except OSError:
            pass
    print(f"serving on http://localhost:{port}")
    try:
        # the LAN address, for phones; a throwaway UDP "connection" (nothing
        # is sent) is the portable way to learn which interface routes out
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        print(f"        and http://{s.getsockname()[0]}:{port}")
        s.close()
    except OSError:
        pass
    server.serve_forever()
