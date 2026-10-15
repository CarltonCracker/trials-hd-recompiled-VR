#!/usr/bin/env python3
"""Serve a locally built web port; never publish builds containing game code."""

import argparse
import functools
import http.server
import io
import os
from pathlib import Path
from urllib.parse import urlsplit


ASSETS_PREFIX = "/assets/"


class Handler(http.server.SimpleHTTPRequestHandler):
    # Development (--assets): the user's own extracted game, served read-only
    # under /assets/ with a generated manifest.txt ("<size> <path>" per line)
    # that the web build mounts lazily (docs/web-port/README.md 8.6, WM1).
    assets = None

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
        root = self.assets if self.is_assets_request() else Path(self.directory)
        return path.resolve().is_relative_to(root.resolve())

    def is_assets_request(self):
        return self.assets is not None and urlsplit(self.path).path.startswith(ASSETS_PREFIX)

    def assets_parts(self):
        # WasmFS's fetch backend asks for /assets//<path>: ignore empty parts.
        relative = urlsplit(self.path).path[len(ASSETS_PREFIX):]
        return [part for part in relative.split("/") if part not in ("", ".", "..")]

    def translate_path(self, path):
        if self.is_assets_request():
            return str(self.assets.joinpath(*self.assets_parts()))
        return super().translate_path(path)

    def send_manifest(self):
        lines = []
        for file in sorted(self.assets.rglob("*")):
            if file.is_file() and self.contained(file):
                lines.append(f"{file.stat().st_size} {file.relative_to(self.assets).as_posix()}\n")
        body = "".join(lines).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        return io.BytesIO(body)

    def send_head(self):
        if self.is_assets_request() and self.assets_parts() == ["manifest.txt"]:
            return self.send_manifest()
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
    parser.add_argument("--assets", type=Path,
                        help="development: serve your extracted game folder under /assets/")
    args = parser.parse_args()
    directory = Path(args.directory).resolve()
    if not directory.is_dir():
        parser.error(f"build directory does not exist: {directory}")
    if args.assets is not None:
        if not args.assets.is_dir():
            parser.error(f"assets directory does not exist: {args.assets}")
        Handler.assets = args.assets.resolve()
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
