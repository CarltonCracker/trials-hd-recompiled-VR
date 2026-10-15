#!/usr/bin/env python3
"""Serve a locally built web port; never publish builds containing game code."""

import argparse
import functools
import http.server
import os
from pathlib import Path
from urllib.parse import urlsplit


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
    }

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def list_directory(self, path):
        self.send_error(404, "Directory listing disabled")
        return None

    def contained(self, path):
        return path.resolve().is_relative_to(Path(self.directory).resolve())

    def send_head(self):
        path = Path(self.translate_path(self.path))
        if not self.contained(path):
            self.send_error(403, "Path outside build directory")
            return None
        if path.is_dir():
            # Check indexes too: a symlink must not expose files outside the build.
            for name in ("index.html", "index.htm", "trials_hd.html"):
                index = path / name
                if not self.contained(index):
                    self.send_error(403, "Path outside build directory")
                    return None
                if index.is_file():
                    if not urlsplit(self.path).path.endswith("/"):
                        return super().send_head()  # Standard slash redirect.
                    path = index
                    break
            else:
                return self.list_directory(path)

        content_type = self.guess_type(str(path))
        encoding = None
        accepted = {}
        for item in self.headers.get("Accept-Encoding", "").split(","):
            name, *parameters = item.strip().lower().split(";")
            quality = 1.0
            for parameter in parameters:
                if parameter.strip().startswith("q="):
                    try:
                        quality = float(parameter.strip()[2:])
                    except ValueError:
                        quality = 0
            accepted[name] = quality if 0 <= quality <= 1 else 0
        candidates = sorted(
            ((accepted.get(name, accepted.get("*", 0)), name, suffix)
             for name, suffix in (("br", ".br"), ("gzip", ".gz"))),
            key=lambda candidate: candidate[0], reverse=True,
        )
        if path.is_file():
            for quality, name, suffix in candidates:
                compressed = Path(str(path) + suffix)
                if quality > 0 and self.contained(compressed) and compressed.is_file():
                    path, encoding = compressed, name
                    break
        try:
            stream = path.open("rb")
        except OSError:
            self.send_error(404, "File not found")
            return None
        stat = os.fstat(stream.fileno())
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(stat.st_size))
        self.send_header("Last-Modified", self.date_time_string(stat.st_mtime))
        self.send_header("Vary", "Accept-Encoding")
        if encoding:
            self.send_header("Content-Encoding", encoding)
        self.end_headers()
        return stream


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", default="out/build/web-wasm64-release/web")
    parser.add_argument("--bind", default="127.0.0.1", help="listen address (default: loopback only)")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    directory = Path(args.directory).resolve()
    if not directory.is_dir():
        parser.error(f"build directory does not exist: {directory}")
    handler = functools.partial(Handler, directory=str(directory))
    with http.server.ThreadingHTTPServer((args.bind, args.port), handler) as server:
        print(f"Serving {directory} at http://{args.bind}:{server.server_port}/", flush=True)
        print("Local/private use only: builds contain recompiled game code.", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
