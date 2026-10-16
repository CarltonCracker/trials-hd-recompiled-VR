"""Finds where web-port threads are stuck: runs until a console pattern, then asks
every worker whether it can still run JS and pauses it to print its JS/wasm stack.

  .venv/bin/python tools/web/perf/worker_stacks.py \
      'http://127.0.0.1:8080/trials_hd.html?web_assets_url=/assets' \
      'no guest swap for 3' 150 out/stall.log tests/headless/controller_ride.txt

Output: OUT.log (console) and OUT.log.stacks. A worker blocked in an infinite
futex wait neither answers nor pauses ("BUSY", "did not pause"); then add
SetWebPhase markers (src/gpu_webgpu/web_phase.h) to narrow it down."""
import asyncio
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/web"))
import cdp  # noqa: E402

workers = {}
paused = {}


class StackSession(cdp.Session):
    async def attach(self, session, kind):
        workers[session] = kind
        await super().attach(session, kind)

    async def pump(self):
        async for raw in self.ws:
            message = json.loads(raw)
            method = message.get("method")
            session = message.get("sessionId")
            params = message.get("params", {})
            if "id" in message:
                future = self.pending.pop(message["id"], None)
                if future and not future.done():
                    if "error" in message:
                        future.set_exception(RuntimeError(message["error"]))
                    else:
                        future.set_result(message.get("result", {}))
                continue
            if method == "Debugger.paused":
                paused[session] = params
            elif method == "Target.attachedToTarget":
                asyncio.ensure_future(self.attach(params["sessionId"],
                                                  params["targetInfo"].get("type", "")))
            elif method == "Runtime.consoleAPICalled":
                args = params.get("args", [])
                text = " ".join(str(a.get("value", a.get("description", ""))) for a in args)
                self.write("console", text)
            elif method == "Runtime.exceptionThrown":
                details = params.get("exceptionDetails", {})
                self.write("exception", details.get("exception", {}).get(
                    "description", details.get("text", "")))


async def dump(s, out):
    asyncio.ensure_future(s.pump())
    # First: which workers can still run JS (idle event loop or JSPI-suspended)?
    async def probe(session):
        try:
            r = await asyncio.wait_for(s.call("Runtime.evaluate", {"expression": "JSON.stringify({name: globalThis.name, brp: globalThis.__brp || 0})", "returnByValue": True}, session=session), 3)
            return r.get("result", {}).get("value")
        except Exception as e:  # noqa: BLE001
            return f"BUSY ({type(e).__name__})"
    results = await asyncio.gather(*(probe(x) for x in workers))
    for (session, kind), r in zip(workers.items(), results):
        print(f"-- probe {kind} {session}: {r}", file=out)
    for session, kind in list(workers.items()):
        try:
            await asyncio.wait_for(s.call("Debugger.enable", session=session), 3)
            await asyncio.wait_for(s.call("Debugger.pause", session=session), 3)
        except Exception as e:  # noqa: BLE001
            print(f"{session} {kind}: {e}", file=out)
    await asyncio.sleep(4)
    for session, kind in workers.items():
        p = paused.get(session)
        if not p:
            try:
                r = await asyncio.wait_for(s.call("Runtime.evaluate", {"expression": "JSON.stringify({name: globalThis.name, brp: globalThis.__brp, gpu: !!(globalThis.WebGPU && WebGPU.Internals && WebGPU.Internals.jsObjects && WebGPU.Internals.jsObjects.length)})", "returnByValue": True}, session=session), 3)
                desc = r.get("result", {}).get("value")
            except Exception as e:  # noqa: BLE001
                desc = f"evaluate failed: {type(e).__name__} {e}"
            print(f"== {kind} {session}: did not pause; {desc}", file=out)
            continue
        print(f"== {kind} {session}: reason {p.get('reason')}", file=out)
        for frame in p["callFrames"][:40]:
            print(f"   {frame.get('functionName')}  {frame.get('url', '')[-40:]}", file=out)
        try:
            await asyncio.wait_for(s.call("Debugger.resume", session=session), 3)
        except Exception:  # noqa: BLE001
            pass


async def main():
    args = cdp.argparse.Namespace(
        url=sys.argv[1], seconds=float(sys.argv[3]), until=sys.argv[2],
        console=Path(sys.argv[4]), eval=None, screenshot=None,
        script=Path(sys.argv[5]) if len(sys.argv) > 5 else None,
        init_js=None, out_dir=Path("out/claude-run/stacks"), viewport="1280x720",
        chromium="chromium")
    orig_drive = cdp.drive

    async def drive(s, a):
        try:
            await orig_drive(s, a)
        except RuntimeError as e:
            print("drive:", e)
        with open(sys.argv[4] + ".stacks", "w") as out:
            await dump(s, out)
        return 0
    cdp.drive = drive
    cdp.Session = StackSession
    await cdp.run(args)

asyncio.run(main())
