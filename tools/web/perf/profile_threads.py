"""CPU-profiles every game thread (worker) of the web port for a window of a run.

  .venv/bin/python tools/web/perf/profile_threads.py \
      'http://127.0.0.1:8080/trials_hd.html?web_assets_url=/assets' \
      tests/headless/ride_middle_name_danger.txt 47 20 out/prof

Runs the script (cdp.py commands), starts the V8 sampling profiler on every worker
START_S seconds after loading, stops it DURATION_S later, writes OUT_DIR/<n>.cpuprofile
(open in Chrome DevTools > Performance) and prints the busiest workers' top
self-time functions (guest functions are named __imp__sub_<address>). "(program)"
is V8-internal time, mostly blocked waits."""
import asyncio
import collections
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/web"))
import cdp  # noqa: E402

workers = {}


class ProfSession(cdp.Session):
    async def attach(self, session, kind):
        workers[session] = kind
        await super().attach(session, kind)


def summarize(profile):
    nodes = {n["id"]: n for n in profile["nodes"]}
    self_time = collections.Counter()
    samples = profile.get("samples", [])
    deltas = profile.get("timeDeltas", [])
    total = 0
    for node_id, dt in zip(samples, deltas):
        name = nodes[node_id]["callFrame"]["functionName"] or "(anon)"
        self_time[name] += dt
        total += dt
    idle = sum(v for k, v in self_time.items() if k in ("(idle)", "(program)", "(garbage collector)"))
    return total, idle, self_time


async def main():
    url, script, start_s, duration_s, out_dir = sys.argv[1:6]
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    args = cdp.argparse.Namespace(
        url=url, seconds=float(start_s) + float(duration_s) + 30, until=None,
        console=out_dir / "console.log", eval=None, screenshot=None, script=Path(script),
        init_js=None, out_dir=out_dir / "captures", viewport="1280x720", chromium="chromium",
        profile=None)
    orig_drive = cdp.drive

    async def drive(s, a):
        task = asyncio.ensure_future(orig_drive(s, a))
        await asyncio.sleep(float(start_s))
        targets = [x for x, k in workers.items() if k == "worker"]
        for x in targets:
            try:
                await asyncio.wait_for(s.call("Profiler.enable", session=x), 5)
                await asyncio.wait_for(s.call("Profiler.setSamplingInterval", {"interval": 500}, session=x), 5)
                await asyncio.wait_for(s.call("Profiler.start", session=x), 5)
            except Exception as e:  # noqa: BLE001
                print("start failed", x, e)
        await asyncio.sleep(float(duration_s))
        results = []
        for i, x in enumerate(targets):
            try:
                r = await asyncio.wait_for(s.call("Profiler.stop", session=x), 30)
            except Exception as e:  # noqa: BLE001
                print("stop failed", x, e)
                continue
            prof = r["profile"]
            (out_dir / f"{i}.cpuprofile").write_text(json.dumps(prof))
            total, idle, st = summarize(prof)
            results.append((total - idle, total, i, st))
        results.sort(reverse=True)
        for busy, total, i, st in results[:8]:
            print(f"== worker {i}: busy {busy/1e6:.2f}s of {total/1e6:.2f}s")
            for name, t in st.most_common(12):
                print(f"   {t/1e6:7.3f}s  {name[:110]}")
        try:
            await task
        except Exception as e:  # noqa: BLE001
            print("drive:", e)
        return 0

    cdp.drive = drive
    cdp.Session = ProfSession
    await cdp.run(args)

asyncio.run(main())
