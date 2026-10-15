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

from websockets.asyncio.client import connect

CHROMIUM_FLAGS = [
    "--headless=new", "--no-sandbox", "--disable-gpu-sandbox", "--enable-unsafe-webgpu",
    "--ignore-gpu-blocklist", "--use-gl=angle", "--use-angle=vulkan",
    "--enable-features=Vulkan", "--use-vulkan=native", "--no-first-run",
    "--no-default-browser-check", "--window-size=1280,720",
]
RADV_ICD = "/usr/share/vulkan/icd.d/radeon_icd.json"


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
            await self.call("Runtime.runIfWaitingForDebugger", session=session)
        except RuntimeError:
            pass


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
    await s.call("Page.navigate", {"url": args.url}, session=s.page_session)
    started = time.monotonic()
    exit_code = None
    while time.monotonic() - started < args.seconds and s.matched is None:
        await asyncio.sleep(1)
        state = await s.call("Runtime.evaluate", {
            "expression": "window.trialsHd ? window.trialsHd.exitCode : null",
            "returnByValue": True}, session=s.page_session)
        exit_code = state.get("result", {}).get("value")
        if exit_code is not None:
            break
    elapsed = time.monotonic() - started
    result = None
    if args.eval:
        value = await s.call("Runtime.evaluate", {
            "expression": args.eval, "returnByValue": True, "awaitPromise": True},
            session=s.page_session)
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
    return 0 if (s.matched is not None or not args.until) else 1


async def run(args):
    chromium = shutil.which(args.chromium) or args.chromium
    port = free_port()
    profile = tempfile.mkdtemp(prefix="trials-hd-chromium-")
    env = dict(os.environ)
    if Path(RADV_ICD).exists():
        env["VK_ICD_FILENAMES"] = RADV_ICD
    browser = subprocess.Popen(
        [chromium, *CHROMIUM_FLAGS, f"--remote-debugging-port={port}",
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
    parser.add_argument("--chromium", default="chromium")
    args = parser.parse_args()
    sys.exit(asyncio.run(run(args)))


if __name__ == "__main__":
    main()
