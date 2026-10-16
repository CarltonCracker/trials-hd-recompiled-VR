"""Traces the first seconds of a page with Chrome tracing (V8 and wasm categories).

  .venv/bin/python tools/web/perf/trace_v8.py \
      'http://127.0.0.1:8080/trials_hd.html?web_assets_url=/assets' 0 14 out/trace.json

Prints the total duration of the top trace events across all threads (e.g.
wasm.CompileLazy, wasm.TopTierCompilation, wasm.SyncInstantiate) and writes the raw
events (load OUT.json in chrome://tracing or Perfetto)."""
import asyncio
import collections
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/web"))
import cdp  # noqa: E402

events = []
done = asyncio.Event()


class TraceSession(cdp.Session):
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
            if method == "Tracing.dataCollected":
                events.extend(params["value"])
            elif method == "Tracing.tracingComplete":
                done.set()
            elif method == "Target.attachedToTarget":
                asyncio.ensure_future(self.attach(params["sessionId"],
                                                  params["targetInfo"].get("type", "")))
            elif method == "Runtime.consoleAPICalled":
                args = params.get("args", [])
                self.write("console", " ".join(str(a.get("value", "")) for a in args))


async def main():
    url, start_s, duration_s, out = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
    args = cdp.argparse.Namespace(url=url, seconds=start_s + duration_s + 20, until=None,
                                  console=Path(out + ".console.log"), eval=None, screenshot=None,
                                  script=None, init_js=None, out_dir=Path("out/run"),
                                  viewport="1280x720", chromium="chromium", profile=None,
                                  chromium_flag=[])

    async def drive(s, a):
        pump = asyncio.ensure_future(s.pump())
        browser_session = None
        await s.call("Tracing.start", {"traceConfig": {"includedCategories": [
            "disabled-by-default-v8.runtime_stats", "v8", "v8.wasm",
            "disabled-by-default-v8.wasm.detailed", "toplevel"]},
            "transferMode": "ReportEvents"})
        target = await s.call("Target.createTarget", {"url": "about:blank"})
        attached = await s.call("Target.attachToTarget", {"targetId": target["targetId"], "flatten": True})
        s.page_session = attached["sessionId"]
        await s.call("Runtime.enable", session=s.page_session)
        await s.call("Target.setAutoAttach", {"autoAttach": True, "waitForDebuggerOnStart": True,
                                              "flatten": True}, session=s.page_session)
        await s.call("Page.navigate", {"url": url}, session=s.page_session)
        await asyncio.sleep(start_s + duration_s)
        await s.call("Tracing.end")
        await asyncio.wait_for(done.wait(), 120)
        pump.cancel()
        Path(out).write_text(json.dumps(events))
        stats = collections.Counter()
        counts = collections.Counter()
        names = collections.Counter()
        for e in events:
            rcs = (e.get("args") or {}).get("runtime-call-stats")
            if rcs:
                for k, v in rcs.items():
                    counts[k] += v[0]
                    stats[k] += v[1]
            if e.get("ph") == "X":
                names[e.get("name")] += e.get("dur", 0)
        print("top runtime-call-stats (ms):")
        for k, v in stats.most_common(30):
            print(f"  {v/1000:9.1f}  {counts[k]:9d}  {k}")
        print("top complete events (ms):")
        for k, v in names.most_common(20):
            print(f"  {v/1000:9.1f}  {k}")
        return 0

    cdp.drive = drive
    cdp.Session = TraceSession
    await cdp.run(args)

asyncio.run(main())
