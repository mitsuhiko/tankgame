#!/usr/bin/env python3
import argparse
import json
import sys
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


class WebHandler(SimpleHTTPRequestHandler):
    base_directory = "."

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=self.base_directory, **kwargs)

    def do_GET(self):
        if self.path in ("", "/"):
            self.send_response(HTTPStatus.FOUND)
            self.send_header("Location", "/tankgame.html")
            self.end_headers()
            return
        super().do_GET()

    def do_OPTIONS(self):
        if self.path == "/api/console":
            self.send_response(HTTPStatus.NO_CONTENT)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.end_headers()
            return
        self.send_error(HTTPStatus.NOT_FOUND, "Not Found")

    def do_POST(self):
        if self.path != "/api/console":
            self.send_error(HTTPStatus.NOT_FOUND, "Not Found")
            return

        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b""
        stream = "stdout"
        message = ""

        if body:
            try:
                payload = json.loads(body.decode("utf-8"))
                stream = payload.get("stream", stream)
                message = payload.get("message", "")
            except (json.JSONDecodeError, UnicodeDecodeError):
                message = body.decode("utf-8", errors="replace")

        output = sys.stderr if stream == "stderr" else sys.stdout
        if message:
            output.write(message)
            if not message.endswith("\n"):
                output.write("\n")
            output.flush()

        self.send_response(HTTPStatus.NO_CONTENT)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

    def log_message(self, format, *args):
        sys.stdout.write("%s - - [%s] %s\n" % (self.client_address[0], self.log_date_time_string(), format % args))


def main():
    parser = argparse.ArgumentParser(description="Serve web build with console forwarding.")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--directory", default=".")
    args = parser.parse_args()

    handler = WebHandler
    handler.base_directory = args.directory

    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    print(f"Serving {args.directory} on http://127.0.0.1:{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server")


if __name__ == "__main__":
    main()
