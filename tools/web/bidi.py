#!/usr/bin/env python3
"""Runs a page of the web port in headless Firefox over WebDriver BiDi (WM7).

The Firefox counterpart of cdp.py: launches Firefox with a fresh profile (WebGPU
enabled; it is off by default on Linux), opens the URL, writes the page's console
to a file, runs tests/headless/*.txt scripts (wait, pad, axis, tap, capture,
record_audio) and can evaluate an expression at the end. Logs and captures stay in
out/ (never commit them: they can contain game data).

  xvfb-run -a -s '-screen 0 1600x1000x24' .venv/bin/python tools/web/bidi.py \\
      'http://127.0.0.1:8080/trials_hd.html?web_assets_url=/assets' --headful \\
      --firefox ~/firefox/firefox --script tests/headless/main_menu_nav.txt \\
      --out-dir out/ff-run --console out/ff-run/console.log

Use --headful under Xvfb whenever captures matter: Firefox's own screenshots are
black for a WebGPU canvas presented by a worker, so captures grab the display.
Firefox's stdout (all console messages, workers included) goes to
<console>.firefox.log.
"""

import argparse
import asyncio
import base64
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time
import wave

from websockets.asyncio.client import connect

PREFS = {
    "dom.webgpu.enabled": True,
    "gfx.webgpu.ignore-blocklist": True,
    # Audio (W-L12) starts without a user gesture in tests.
    "media.autoplay.default": 0,
    "media.autoplay.blocking_policy": 0,
    # Every console message (workers too) also goes to Firefox's stdout.
    "devtools.console.stdout.content": True,
}
PAD_BUTTON_MAP = {
    "a": 0, "b": 1, "x": 2, "y": 3, "lb": 4, "rb": 5, "lt": 6, "rt": 7,
    "back": 8, "start": 9, "ls": 10, "rs": 11, "up": 12, "down": 13, "left": 14, "right": 15,
}
# WebDriver key codes for the named keys the scripts use.
KEYS = {
    "Return": "", "Escape": "", "BackSpace": "", "Tab": "",
    "space": " ", "Up": "", "Down": "", "Left": "", "Right": "",
    **{f"F{i}": chr(0xE031 + i - 1) for i in range(1, 13)},
}


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Bidi:
    def __init__(self, ws, console):
        self.ws = ws
        self.console = console
        self.next_id = 0
        self.pending = {}
        self.lines = 0
        self.pattern = None
        self.matched = None
        self.context = None
        self.headful = False

    async def call(self, method, params=None):
        self.next_id += 1
        future = asyncio.get_running_loop().create_future()
        self.pending[self.next_id] = future
        await self.ws.send(json.dumps({"id": self.next_id, "method": method,
                                       "params": params or {}}))
        return await future

    def write(self, origin, text):
        stamp = time.strftime("%H:%M:%S")
        for line in str(text).splitlines() or [""]:
            print(f"{stamp} [{origin}] {line}", file=self.console, flush=True)
            self.lines += 1
            if self.pattern and self.matched is None and self.pattern.search(line):
                self.matched = line

    async def pump(self):
        async for raw in self.ws:
            message = json.loads(raw)
            if "id" in message:
                future = self.pending.pop(message["id"], None)
                if future and not future.done():
                    if message.get("type") == "error":
                        future.set_exception(RuntimeError(f"{message.get('error')}: "
                                                          f"{message.get('message')}"))
                    else:
                        future.set_result(message.get("result", {}))
            elif message.get("method") == "log.entryAdded":
                entry = message["params"]
                self.write(entry.get("level", "log"), entry.get("text", ""))

    async def evaluate(self, expression, await_promise=True):
        result = await self.call("script.evaluate", {
            "expression": expression, "target": {"context": self.context},
            "awaitPromise": await_promise, "resultOwnership": "none"})
        if result.get("type") == "exception":
            raise RuntimeError(f"evaluation failed: {result.get('exceptionDetails', {}).get('text')}")
        value = result.get("result", {})
        return value.get("value")

    async def key(self, key, ms):
        value = KEYS.get(key, key)
        await self.call("input.performActions", {"context": self.context, "actions": [{
            "type": "key", "id": "keyboard", "actions": [
                {"type": "keyDown", "value": value}, {"type": "pause", "duration": int(ms)},
                {"type": "keyUp", "value": value}]}]})

    async def click(self, x, y):
        await self.call("input.performActions", {"context": self.context, "actions": [{
            "type": "pointer", "id": "mouse", "parameters": {"pointerType": "mouse"},
            "actions": [{"type": "pointerMove", "x": int(x), "y": int(y)},
                        {"type": "pointerDown", "button": 0}, {"type": "pause", "duration": 50},
                        {"type": "pointerUp", "button": 0}]}]})


async def capture_screen(b, out_file):
    """Grabs the page area from the X display. Firefox's own screenshots are
    black for a WebGPU canvas that a worker presents (OffscreenCanvas)."""
    box = json.loads(await b.evaluate(
        "JSON.stringify([mozInnerScreenX, mozInnerScreenY, innerWidth, innerHeight,"
        " devicePixelRatio])", False))
    x, y, width, height, dpr = box
    proc = await asyncio.create_subprocess_exec(
        "ffmpeg", "-loglevel", "error", "-y", "-f", "x11grab",
        "-video_size", f"{round(width * dpr)}x{round(height * dpr)}",
        "-i", f"{os.environ['DISPLAY']}+{round(x * dpr)},{round(y * dpr)}",
        "-frames:v", "1", str(out_file))
    if await proc.wait() != 0:
        raise RuntimeError("ffmpeg could not grab the display")


async def run_script(b, script_path, out_dir):
    lines = [line.split("#")[0].split() for line in Path(script_path).read_text().splitlines()]
    for parts in (l for l in lines if l):
        cmd, args = parts[0], parts[1:]
        if cmd == "wait":
            await asyncio.sleep(float(args[0]))
        elif cmd == "tap":
            await b.key(args[0], float(args[1]) if len(args) > 1 else 80)
        elif cmd == "pad":
            await b.evaluate(f"window.setSyntheticPadButton({PAD_BUTTON_MAP.get(args[0].lower(), 0)}, "
                             f"{int(args[1])})", False)
        elif cmd == "axis":
            await b.evaluate(f"window.setSyntheticPadAxis('{args[0].lower()}', {float(args[1])})", False)
        elif cmd == "click":
            await b.click(float(args[0]), float(args[1]))
        elif cmd == "focus":
            await b.evaluate("document.querySelector('canvas')?.focus()", False)
        elif cmd == "capture":
            out_file = Path(out_dir) / f"{args[0]}.png"
            out_file.parent.mkdir(parents=True, exist_ok=True)
            if b.headful:
                await capture_screen(b, out_file)
            else:
                shot = await b.call("browsingContext.captureScreenshot", {"context": b.context})
                out_file.write_bytes(base64.b64decode(shot["data"]))
            b.write("capture", f"saved screenshot to {out_file}")
        elif cmd == "record_audio":
            expr = (
                "(async () => { const a = window.trialsHd.audio;"
                f" const d = await a.record({float(args[1])});"
                " const pcm = new Int16Array(d.length);"
                " for (let i = 0; i < d.length; ++i) pcm[i] = Math.max(-1, Math.min(1, d[i])) * 32767;"
                " const u8 = new Uint8Array(pcm.buffer); let bin = '';"
                " for (let i = 0; i < u8.length; i += 32768) bin += String.fromCharCode(...u8.subarray(i, i + 32768));"
                " return JSON.stringify({rate: a.context.sampleRate, data: btoa(bin)}); })()")
            recorded = json.loads(await b.evaluate(expr))
            out_file = Path(out_dir) / f"{args[0]}.wav"
            out_file.parent.mkdir(parents=True, exist_ok=True)
            with wave.open(str(out_file), "wb") as wav:
                wav.setnchannels(2)
                wav.setsampwidth(2)
                wav.setframerate(int(recorded["rate"]))
                wav.writeframes(base64.b64decode(recorded["data"]))
            b.write("capture", f"saved audio to {out_file}")


async def drive(b, args):
    pump = asyncio.ensure_future(b.pump())
    b.headful = args.headful
    await b.call("session.new", {"capabilities": {}})
    await b.call("session.subscribe", {"events": ["log.entryAdded"]})
    tree = await b.call("browsingContext.getTree", {})
    b.context = tree["contexts"][0]["context"]
    width, height = (int(v) for v in args.viewport.lower().split("x"))
    await b.call("browsingContext.setViewport", {
        "context": b.context, "viewport": {"width": width, "height": height}})
    b.pattern = re.compile(args.until) if args.until else None
    await b.call("browsingContext.navigate", {"context": b.context, "url": args.url,
                                              "wait": "complete"})
    started = time.monotonic()
    script = asyncio.ensure_future(run_script(b, args.script, args.out_dir)) if args.script else None
    exit_code = None
    while time.monotonic() - started < args.seconds and b.matched is None:
        await asyncio.sleep(1)
        exit_code = await b.evaluate("window.trialsHd ? window.trialsHd.exitCode : null", False)
        if exit_code is not None or (script and script.done() and not args.until):
            break
    if script:
        if not script.done():
            script.cancel()
            raise RuntimeError("Headless script did not complete before the deadline")
        await script
    result = await b.evaluate(args.eval) if args.eval else None
    pump.cancel()
    print(f"{time.monotonic() - started:.1f}s, {b.lines} console lines -> {args.console}")
    if args.until:
        print(f"matched: {b.matched}" if b.matched else f"pattern not seen: {args.until}")
    if exit_code is not None:
        print(f"page reported exit code {exit_code}")
    if args.eval:
        print("eval:", json.dumps(result))
    return 0 if (b.matched or not args.until) else 1


async def run(args):
    port = free_port()
    profile = tempfile.mkdtemp(prefix="trials-hd-firefox-")
    Path(profile, "user.js").write_text("".join(
        f'user_pref("{k}", {json.dumps(v)});\n' for k, v in PREFS.items()))
    args.console.parent.mkdir(parents=True, exist_ok=True)
    browser_log = args.console.with_suffix(".firefox.log").open("w")
    browser = subprocess.Popen(
        [args.firefox, *([] if args.headful else ["--headless"]), "--no-remote", "--profile", profile,
         f"--remote-debugging-port={port}", "about:blank"],
        stdout=browser_log, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 30
        while True:
            try:
                ws = await connect(f"ws://127.0.0.1:{port}/session", max_size=256 * 1024 * 1024)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise SystemExit("Firefox did not start (no WebDriver BiDi endpoint)")
                await asyncio.sleep(0.5)
        args.console.parent.mkdir(parents=True, exist_ok=True)
        with args.console.open("w") as console:
            async with ws:
                return await drive(Bidi(ws, console), args)
    finally:
        browser.terminate()
        try:
            browser.wait(10)
        except subprocess.TimeoutExpired:
            browser.kill()
        shutil.rmtree(profile, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("url")
    parser.add_argument("--firefox", default="firefox")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--until", help="stop when a console line matches this regex")
    parser.add_argument("--console", type=Path, default=Path("out/ff-run/console.log"))
    parser.add_argument("--eval", help="JavaScript expression to evaluate at the end")
    parser.add_argument("--script", type=Path, help="run a tests/headless/*.txt script")
    parser.add_argument("--out-dir", type=Path, default=Path("out/ff-run"))
    parser.add_argument("--viewport", default="1280x720", help="page size in CSS pixels")
    parser.add_argument("--headful", action="store_true",
                        help="show a window instead of --headless, under xvfb-run; captures then "
                             "grab the display (Firefox's screenshots miss worker WebGPU canvases)")
    raise SystemExit(asyncio.run(run(parser.parse_args())))


if __name__ == "__main__":
    main()
