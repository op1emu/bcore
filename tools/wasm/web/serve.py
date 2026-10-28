#!/usr/bin/env python3
"""Serves a directory (the web/ files, emu-web.js and .wasm, programs/) on
127.0.0.1:PORT; a POST to /result is written to result.json there.

  serve.py PORT [DIR]
"""
import http.server
import os
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".wasm": "application/wasm", ".js": "text/javascript"}

    def do_POST(self):
        length = int(self.headers["Content-Length"])
        with open("result.json", "wb") as f:
            f.write(self.rfile.read(length))
        self.send_response(204)
        self.end_headers()

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    if len(sys.argv) > 2:
        os.chdir(sys.argv[2])
    http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
