"""Regression checks for the runtime-rate log checker; run with Python 3."""
import pathlib
import subprocess
import sys
import tempfile

checker = pathlib.Path(__file__).with_name("check_fps_log.py")
sample = ("fixed updates/sec: 60.0 real renders/sec: 120.0\n"
          "guest fps: 120.0 draws/frame: 72.0 resolves/frame: 1.0\n")
with tempfile.TemporaryDirectory() as directory:
    log = pathlib.Path(directory) / "run.log"
    for text, valid in [
        (sample * 30, True),
        (sample * 5, False),
        (sample.replace("60.0", "120.0") * 30, False),
        (sample.replace("draws/frame: 72.0", "draws/frame: 0.0") * 30, False),
        (sample.replace("guest fps: 120.0", "guest fps: 60.0") * 30, False),
        (sample * 30 + "Unhandled guest access violation", False),
    ]:
        log.write_text(text)
        result = subprocess.run([sys.executable, str(checker), str(log), "120"],
                                capture_output=True, text=True)
        assert (result.returncode == 0) == valid, result.stdout + result.stderr
print("runtime-rate log checks passed")
