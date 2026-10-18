#!/usr/bin/env python3
"""Runs a page of the web port in headless Chromium (docs/web-port/README.md 11.1-11.2).

Launches Chromium with hardware WebGPU on the RX 6600, opens the URL, and writes the
console of the page and of all its workers (the game's pthreads) to a file until a
pattern appears, the page's game exits, or time runs out. Optionally evaluates a
JavaScript expression and saves a screenshot at the end. Logs stay in out/ (never
commit them: they can contain game data).

  .venv/bin/python tools/web/cdp.py 'http://127.0.0.1:8080/?log_level=info' \\
      --seconds 90 --until 'first guest swap' --console out/web-run/console.log
"""

import argparse
import asyncio
import base64
import contextlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import wave

from websockets.asyncio.client import connect

CHROMIUM_FLAGS = [
    "--headless=new", "--no-sandbox", "--disable-gpu-sandbox", "--enable-unsafe-webgpu",
    "--ignore-gpu-blocklist", "--use-gl=angle", "--use-angle=vulkan",
    "--enable-features=Vulkan", "--use-vulkan=native", "--no-first-run",
    "--no-default-browser-check", "--window-size=1280,720",
    # Audio (W-L12) starts without a user gesture in tests.
    "--autoplay-policy=no-user-gesture-required",
]
RADV_ICD = "/usr/share/vulkan/icd.d/radeon_icd.json"


def amd_render_node():
    for node in Path("/dev/dri").glob("renderD*"):
        vendor = Path("/sys/class/drm") / node.name / "device" / "vendor"
        if vendor.exists() and vendor.read_text().strip() == "0x1002":
            return True
    return False


def start_xvfb():
    """A private X display for a headful browser; returns (process, DISPLAY)."""
    if not shutil.which("Xvfb"):
        sys.exit("headful runs need Xvfb (apt install xvfb), or pass --headless")
    read_fd, write_fd = os.pipe()
    xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1920x1080x24",
                             "-nolisten", "tcp"], pass_fds=(write_fd,),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(write_fd)
    with os.fdopen(read_fd) as f:
        display = f.readline().strip()
    if not display:
        xvfb.kill()
        sys.exit("Xvfb did not start")
    return xvfb, f":{display}"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Session:
    """One websocket to the browser; CDP sessions are multiplexed by sessionId."""

    def __init__(self, ws, console):
        self.ws = ws
        self.console = console
        self.next_id = 0
        self.pending = {}
        self.lines = 0
        self.matched = None
        self.pattern = None
        self.exceptions = 0
        self.page_session = None

    async def call(self, method, params=None, session=None):
        self.next_id += 1
        message = {"id": self.next_id, "method": method, "params": params or {}}
        if session:
            message["sessionId"] = session
        future = asyncio.get_running_loop().create_future()
        self.pending[self.next_id] = future
        await self.ws.send(json.dumps(message))
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
                    if "error" in message:
                        future.set_exception(RuntimeError(message["error"]))
                    else:
                        future.set_result(message.get("result", {}))
                continue
            method = message.get("method")
            params = message.get("params", {})
            session = message.get("sessionId")
            if method == "Target.attachedToTarget":
                info = params["targetInfo"]
                child = params["sessionId"]
                asyncio.ensure_future(self.attach(child, info.get("type", "")))
            elif method == "Runtime.consoleAPICalled":
                args = params.get("args", [])
                text = " ".join(str(a.get("value", a.get("description", ""))) for a in args)
                self.write("console" if session == self.page_session else "worker", text)
            elif method == "Runtime.exceptionThrown":
                self.exceptions += 1
                details = params.get("exceptionDetails", {})
                exception = details.get("exception", {})
                self.write("exception", exception.get("description", details.get("text", "")))
            elif method == "Log.entryAdded":
                entry = params.get("entry", {})
                self.write(f"log:{entry.get('level')}", entry.get("text", ""))

    async def attach(self, session, kind):
        # Every worker (the game's pthreads) is attached too, so their console
        # output and exceptions are captured; then it is released to run.
        try:
            await self.call("Runtime.enable", session=session)
            await self.call("Target.setAutoAttach", {
                "autoAttach": True, "waitForDebuggerOnStart": True, "flatten": True},
                session=session)
        except RuntimeError as error:
            # Some targets (the AudioWorklet) do not take every command; they
            # must still be released below, or they stay paused forever.
            self.write("cdp", f"attach {kind}: {error}")
        try:
            await self.call("Runtime.runIfWaitingForDebugger", session=session)
        except RuntimeError:
            pass


PAD_BUTTON_MAP = {
    "a": 0, "b": 1, "x": 2, "y": 3,
    "lb": 4, "rb": 5, "lt": 6, "rt": 7,
    "back": 8, "start": 9,
    "ls": 10, "rs": 11,
    "up": 12, "down": 13, "left": 14, "right": 15,
}

KEYSYM_MAP = {
    "space": ("Space", " "), "Return": ("Enter", "\r"), "Escape": ("Escape", ""),
    "Tab": ("Tab", "\t"), "BackSpace": ("Backspace", ""),
    "Up": ("ArrowUp", ""), "Down": ("ArrowDown", ""),
    "Left": ("ArrowLeft", ""), "Right": ("ArrowRight", ""),
    "F1": ("F1", ""), "F2": ("F2", ""), "F3": ("F3", ""), "F4": ("F4", ""),
    "F5": ("F5", ""), "F6": ("F6", ""), "F7": ("F7", ""), "F8": ("F8", ""),
    "F9": ("F9", ""), "F10": ("F10", ""), "F11": ("F11", ""), "F12": ("F12", ""),
}


async def run_headless_script(s, script_path, out_dir):
    """Executes a tests/headless/*.txt script against the web page via CDP."""
    with open(script_path) as f:
        lines = [line.split("#")[0].strip().split() for line in f]
    lines = [l for l in lines if l]
    for parts in lines:
        cmd, c_args = parts[0], parts[1:]
        if cmd == "wait":
            await asyncio.sleep(float(c_args[0]))
        elif cmd in ("tap", "hold"):
            ms = float(c_args[1]) if len(c_args) > 1 else 80
            key = c_args[0]
            code, text = KEYSYM_MAP.get(key, (f"Key{key.upper()}", key))
            await s.call("Input.dispatchKeyEvent", {
                "type": "rawKeyDown", "code": code, "key": key, "text": text
            }, session=s.page_session)
            await asyncio.sleep(ms / 1000.0)
            await s.call("Input.dispatchKeyEvent", {
                "type": "keyUp", "code": code, "key": key
            }, session=s.page_session)
        elif cmd == "down":
            key = c_args[0]
            code, text = KEYSYM_MAP.get(key, (f"Key{key.upper()}", key))
            await s.call("Input.dispatchKeyEvent", {
                "type": "rawKeyDown", "code": code, "key": key, "text": text
            }, session=s.page_session)
        elif cmd == "up":
            key = c_args[0]
            code, _ = KEYSYM_MAP.get(key, (f"Key{key.upper()}", key))
            await s.call("Input.dispatchKeyEvent", {
                "type": "keyUp", "code": code, "key": key
            }, session=s.page_session)
        elif cmd == "pad":
            btn = c_args[0].lower()
            val = int(c_args[1])
            idx = PAD_BUTTON_MAP.get(btn, 0)
            expr = f"window.setSyntheticPadButton({idx}, {val});"
            result = await s.call("Runtime.evaluate", {"expression": expr}, session=s.page_session)
            if "exceptionDetails" in result:
                raise RuntimeError(f"Controller button injection failed: {result['exceptionDetails']}")
        elif cmd == "axis":
            axis = c_args[0].lower()
            raw_val = float(c_args[1])
            expr = f"window.setSyntheticPadAxis('{axis}', {raw_val});"
            result = await s.call("Runtime.evaluate", {"expression": expr}, session=s.page_session)
            if "exceptionDetails" in result:
                raise RuntimeError(f"Controller axis injection failed: {result['exceptionDetails']}")
        elif cmd == "capture":
            name = c_args[0]
            shot = await s.call("Page.captureScreenshot", {"format": "png"}, session=s.page_session)
            out_file = out_dir / f"{name}.png"
            out_file.parent.mkdir(parents=True, exist_ok=True)
            out_file.write_bytes(base64.b64decode(shot["data"]))
            s.write("capture", f"saved screenshot to {out_file}")
        elif cmd == "record_audio":
            # record_audio NAME SECONDS: saves the next SECONDS of what the page's
            # AudioWorklet plays to NAME.wav (blocks like `wait`).
            name, seconds = c_args[0], float(c_args[1])
            expr = (
                "(async () => { const a = window.trialsHd.audio;"
                f" const d = await a.record({seconds});"
                " const pcm = new Int16Array(d.length);"
                " for (let i = 0; i < d.length; ++i) pcm[i] = Math.max(-1, Math.min(1, d[i])) * 32767;"
                " const u8 = new Uint8Array(pcm.buffer); let bin = '';"
                " for (let i = 0; i < u8.length; i += 32768) bin += String.fromCharCode(...u8.subarray(i, i + 32768));"
                " return JSON.stringify({rate: a.context.sampleRate, data: btoa(bin)}); })()")
            result = await s.call("Runtime.evaluate", {
                "expression": expr, "awaitPromise": True, "returnByValue": True},
                session=s.page_session)
            if "exceptionDetails" in result:
                raise RuntimeError(f"Audio recording failed: {result['exceptionDetails']}")
            recorded = json.loads(result["result"]["value"])
            out_file = out_dir / f"{name}.wav"
            out_file.parent.mkdir(parents=True, exist_ok=True)
            with wave.open(str(out_file), "wb") as wav:
                wav.setnchannels(2)
                wav.setsampwidth(2)
                wav.setframerate(int(recorded["rate"]))
                wav.writeframes(base64.b64decode(recorded["data"]))
            s.write("capture", f"saved audio to {out_file}")
        elif cmd == "click":
            # click X Y: a left click at CSS pixel (X, Y) of the page.
            x, y = float(c_args[0]), float(c_args[1])
            for kind in ("mouseMoved", "mousePressed", "mouseReleased"):
                await s.call("Input.dispatchMouseEvent", {
                    "type": kind, "x": x, "y": y, "button": "left",
                    "buttons": 1 if kind == "mousePressed" else 0, "clickCount": 1,
                }, session=s.page_session)
                await asyncio.sleep(0.05)
        elif cmd == "focus":
            await s.call("Runtime.evaluate", {
                "expression": "const c = document.querySelector('canvas'); if (c) c.focus();"
            }, session=s.page_session)


async def drive(s, args):
    """Opens the page with every worker attached and waits; prints a summary."""
    s.pattern = re.compile(args.until) if args.until else None
    pump = asyncio.ensure_future(s.pump())
    target = await s.call("Target.createTarget", {"url": "about:blank"})
    attached = await s.call("Target.attachToTarget",
                            {"targetId": target["targetId"], "flatten": True})
    s.page_session = attached["sessionId"]
    for method in ("Runtime.enable", "Log.enable", "Page.enable"):
        await s.call(method, session=s.page_session)
    await s.call("Target.setAutoAttach", {
        "autoAttach": True, "waitForDebuggerOnStart": True, "flatten": True},
        session=s.page_session)
    if args.viewport:
        # An exact CSS viewport (the canvas fills it): 1280x720 makes the canvas
        # the same size as the native guest-output captures.
        size, _, scale = args.viewport.lower().partition("@")
        width, height = (int(v) for v in size.split("x"))
        await s.call("Emulation.setDeviceMetricsOverride", {
            "width": width, "height": height, "deviceScaleFactor": float(scale or 1),
            "mobile": False}, session=s.page_session)
    if getattr(args, "init_js", None):
        await s.call("Page.addScriptToEvaluateOnNewDocument", {
            "source": args.init_js.read_text()}, session=s.page_session)
    await s.call("Page.navigate", {"url": args.url}, session=s.page_session)
    started = time.monotonic()
    exit_code = None

    script_task = None
    if getattr(args, "script", None):
        out_dir = getattr(args, "out_dir", Path("out/run"))
        script_task = asyncio.ensure_future(run_headless_script(s, args.script, out_dir))

    while time.monotonic() - started < args.seconds and s.matched is None:
        await asyncio.sleep(1)
        state = await s.call("Runtime.evaluate", {
            "expression": "window.trialsHd ? window.trialsHd.exitCode : null",
            "returnByValue": True}, session=s.page_session)
        exit_code = state.get("result", {}).get("value")
        if exit_code is not None:
            break
        if script_task and script_task.done():
            # If script completed and no other wait pattern requested, we can finish
            if not args.until:
                break

    if script_task:
        if not script_task.done():
            script_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await script_task
            if exit_code is None:
                raise RuntimeError("Headless script did not complete before the deadline")
        else:
            await script_task

    elapsed = time.monotonic() - started
    result = None
    if args.eval:
        value = await s.call("Runtime.evaluate", {
            "expression": args.eval, "returnByValue": True, "awaitPromise": True},
            session=s.page_session)
        if "exceptionDetails" in value:
            raise RuntimeError(f"Page evaluation failed: {value['exceptionDetails']}")
        result = value.get("result", {}).get("value")
    if args.screenshot:
        shot = await s.call("Page.captureScreenshot", {"format": "png"}, session=s.page_session)
        args.screenshot.parent.mkdir(parents=True, exist_ok=True)
        args.screenshot.write_bytes(base64.b64decode(shot["data"]))
    pump.cancel()
    print(f"{elapsed:.1f}s, {s.lines} console lines -> {args.console}")
    if s.matched is not None:
        print(f"matched: {s.matched}")
    elif args.until:
        print(f"pattern not seen: {args.until}")
    if exit_code is not None:
        print(f"page reported exit code {exit_code}")
    if s.exceptions:
        print(f"{s.exceptions} uncaught exception(s), see the console file")
    if args.eval:
        print("eval:", json.dumps(result))
    success = (s.matched is not None or not args.until) and not s.exceptions
    return 0 if success and exit_code in (None, 0) else 1


async def run(args):
    chromium = shutil.which(args.chromium) or args.chromium
    port = free_port()
    # A kept profile keeps the browser's caches (shaders, OPFS) between runs.
    keep = getattr(args, "profile", None)
    profile = str(keep) if keep else tempfile.mkdtemp(prefix="trials-hd-chromium-")
    env = dict(os.environ)
    # RADV only when this machine has a usable AMD render node (11.1); an explicit
    # VK_ICD_FILENAMES wins, and other GPUs keep the loader's default.
    amd = amd_render_node()
    if "VK_ICD_FILENAMES" not in env and Path(RADV_ICD).exists() and amd:
        env["VK_ICD_FILENAMES"] = RADV_ICD
    flags = list(CHROMIUM_FLAGS)
    # NVIDIA (proprietary driver), measured 2026-10-17: headless Chromium gets either
    # the hardware WebGPU adapter or a composited canvas, never both (page
    # screenshots stay black). A headful browser on its own Xvfb display has both.
    requested = getattr(args, "headful", None)  # tools may build args without it
    headful = requested if requested is not None else not amd
    xvfb = None
    if headful:
        flags.remove("--headless=new")
        if not getattr(args, "viewport", None):
            args.viewport = "1280x720"  # the window also has browser UI
        if "DISPLAY" not in env or requested is None:
            xvfb, env["DISPLAY"] = start_xvfb()
    browser = subprocess.Popen(
        [chromium, *flags, *(getattr(args, "chromium_flag", None) or []),
         f"--remote-debugging-port={port}",
         f"--user-data-dir={profile}", "about:blank"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        endpoint = None
        deadline = time.monotonic() + 30
        while endpoint is None and time.monotonic() < deadline:
            line = await asyncio.to_thread(browser.stderr.readline)
            match = re.search(r"DevTools listening on (ws://\S+)", line)
            if match:
                endpoint = match.group(1)
        if endpoint is None:
            sys.exit("Chromium did not start (no DevTools endpoint)")
        args.console.parent.mkdir(parents=True, exist_ok=True)
        with args.console.open("w") as console:
            async with connect(endpoint, max_size=64 * 1024 * 1024) as ws:
                return await drive(Session(ws, console), args)
    finally:
        browser.terminate()
        try:
            browser.wait(10)
        except subprocess.TimeoutExpired:
            browser.kill()
        if xvfb:
            xvfb.terminate()
        if not keep:
            shutil.rmtree(profile, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("url")
    parser.add_argument("--seconds", type=float, default=60, help="stop after this long")
    parser.add_argument("--until", help="stop when a console line matches this regex")
    parser.add_argument("--console", type=Path, default=Path("out/web-run/console.log"))
    parser.add_argument("--eval", help="JavaScript expression to evaluate at the end")
    parser.add_argument("--screenshot", type=Path, help="save a PNG of the page at the end")
    parser.add_argument("--script", type=Path, help="run a tests/headless/*.txt script against the page")
    parser.add_argument("--init-js", type=Path, help="test fixture to inject before page scripts run")
    parser.add_argument("--out-dir", type=Path, default=Path("out/run"), help="output directory for captures")
    parser.add_argument("--viewport", help="exact page size in CSS pixels, e.g. 1280x720, optionally @DPR (1280x720@2)")
    parser.add_argument("--chromium", default="chromium")
    parser.add_argument("--chromium-flag", action="append", default=[],
                        help="extra Chromium command-line flag (repeatable; experiments)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--headful", dest="headful", action="store_true", default=None,
                      help="visible browser window (on a private Xvfb display when DISPLAY is unset); "
                           "default on machines without an AMD GPU")
    mode.add_argument("--headless", dest="headful", action="store_false",
                      help="headless browser (default with an AMD GPU)")
    parser.add_argument("--profile", type=Path,
                        help="browser profile directory to use and keep (default: a fresh one)")
    args = parser.parse_args()
    sys.exit(asyncio.run(run(args)))


if __name__ == "__main__":
    main()
