#!/usr/bin/env python3
"""Self-contained localhost-server checks; uses no game data or browser packages."""

import functools
import gzip
import http.client
import http.server
from pathlib import Path
import tempfile
import threading
import unittest

from serve import Handler


class QuietHandler(Handler):
    def log_message(self, *args):
        pass


class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name) / "build"
        cls.root.mkdir()
        (cls.root / "trials_hd.html").write_text("<!doctype html><p>local test</p>")
        cls.wasm = b"\0asm\x01\0\0\0"
        (cls.root / "test.wasm").write_bytes(cls.wasm)
        (cls.root / "test.wasm.gz").write_bytes(gzip.compress(cls.wasm))
        # The server only negotiates precompressed bytes; it does not need brotli.
        (cls.root / "test.wasm.br").write_bytes(b"precompressed-brotli-fixture")
        outside = Path(cls.temp.name) / "private.txt"
        outside.write_text("private")
        (cls.root / "escape").symlink_to(outside)
        (cls.root / "empty").mkdir()
        (cls.root / "bad-index").mkdir()
        (cls.root / "bad-index" / "index.html").symlink_to(outside)
        (cls.root / "nested").mkdir()
        (cls.root / "nested" / "index.html").write_text("nested")
        cls.server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), functools.partial(QuietHandler, directory=str(cls.root)))
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        cls.temp.cleanup()

    def request(self, path="/test.wasm", encoding=None, method="GET"):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port)
        connection.request(method, path, headers={} if encoding is None else {"Accept-Encoding": encoding})
        response = connection.getresponse()
        result = response.status, dict(response.getheaders()), response.read()
        connection.close()
        return result

    def test_wasm_and_isolation_headers(self):
        status, headers, body = self.request()
        self.assertEqual((status, body), (200, self.wasm))
        self.assertEqual(headers["Content-Type"], "application/wasm")
        self.assertEqual(headers["Cross-Origin-Opener-Policy"], "same-origin")
        self.assertEqual(headers["Cross-Origin-Embedder-Policy"], "require-corp")
        self.assertEqual(headers["Cross-Origin-Resource-Policy"], "same-origin")

    def test_head(self):
        status, headers, body = self.request(method="HEAD")
        self.assertEqual((status, body), (200, b""))
        self.assertEqual(int(headers["Content-Length"]), len(self.wasm))

    def test_compression(self):
        for encoding in ("gzip", "br"):
            with self.subTest(encoding=encoding):
                status, headers, body = self.request(encoding=encoding)
                self.assertEqual(status, 200)
                self.assertEqual(headers["Content-Encoding"], encoding)
                self.assertEqual(headers["Content-Type"], "application/wasm")
                self.assertEqual(headers["Vary"], "Accept-Encoding")
                if encoding == "gzip":
                    self.assertEqual(gzip.decompress(body), self.wasm)
                else:
                    self.assertEqual(body, b"precompressed-brotli-fixture")

    def test_quality_and_identity(self):
        _, headers, _ = self.request(encoding="br;q=0.1, gzip;q=1")
        self.assertEqual(headers["Content-Encoding"], "gzip")
        for encoding in ("br;q=0, gzip;q=0", "br;q=invalid", "*;q=0"):
            _, headers, body = self.request(encoding=encoding)
            self.assertNotIn("Content-Encoding", headers)
            self.assertEqual(body, self.wasm)

    def test_index_and_redirect(self):
        self.assertEqual(self.request("/")[0], 200)
        status, headers, _ = self.request("/nested?x=1")
        self.assertEqual(status, 301)
        self.assertEqual(headers["Location"], "/nested/?x=1")
        self.assertEqual(self.request("/nested/")[2], b"nested")

    def test_containment_and_no_listing(self):
        for path in ("/escape", "/bad-index/", "/../private.txt", "/%2e%2e/private.txt", "/empty/"):
            with self.subTest(path=path):
                status, headers, body = self.request(path)
                self.assertIn(status, (403, 404))
                self.assertNotIn(b"<pre>private</pre>", body)
                self.assertEqual(headers["Cross-Origin-Embedder-Policy"], "require-corp")

    def test_compressed_symlink_cannot_escape(self):
        (self.root / "test.wasm.gz").unlink()
        (self.root / "test.wasm.gz").symlink_to(Path(self.temp.name) / "private.txt")
        try:
            _, headers, body = self.request(encoding="gzip")
            self.assertNotIn("Content-Encoding", headers)
            self.assertEqual(body, self.wasm)
        finally:
            (self.root / "test.wasm.gz").unlink()
            (self.root / "test.wasm.gz").write_bytes(gzip.compress(self.wasm))


class AssetsTests(unittest.TestCase):
    """--assets: the user's game folder under /assets/, read-only, with a manifest."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name) / "build"
        cls.root.mkdir()
        cls.assets = Path(cls.temp.name) / "game"
        (cls.assets / "media").mkdir(parents=True)
        (cls.assets / "default.xex").write_bytes(b"XEX2" + bytes(12))
        (cls.assets / "media" / "a.bin").write_bytes(b"abc")
        outside = Path(cls.temp.name) / "private.txt"
        outside.write_text("private")
        (cls.assets / "escape").symlink_to(outside)
        handler = type("AssetsHandler", (QuietHandler,), {"assets": cls.assets.resolve()})
        cls.server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), functools.partial(handler, directory=str(cls.root)))
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        cls.temp.cleanup()

    def request(self, path):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port)
        connection.request("GET", path)
        response = connection.getresponse()
        result = response.status, dict(response.getheaders()), response.read()
        connection.close()
        return result

    def test_manifest_lists_files_inside_only(self):
        status, headers, body = self.request("/assets/manifest.txt")
        self.assertEqual(status, 200)
        self.assertEqual(headers["Cross-Origin-Embedder-Policy"], "require-corp")
        self.assertEqual(body.decode(), "16 default.xex\n3 media/a.bin\n")

    def test_fetch_backend_double_slash(self):
        self.assertEqual(self.request("/assets//manifest.txt")[2].decode(),
                         "16 default.xex\n3 media/a.bin\n")
        self.assertEqual(self.request("/assets//media/a.bin")[2], b"abc")

    def test_files_and_containment(self):
        self.assertEqual(self.request("/assets/media/a.bin")[2], b"abc")
        for path in ("/assets/escape", "/assets/../build", "/assets/%2e%2e/private.txt",
                     "/assets/media/"):
            with self.subTest(path=path):
                status, _, body = self.request(path)
                self.assertIn(status, (403, 404))
                self.assertNotIn(b"private", body)


if __name__ == "__main__":
    unittest.main()
