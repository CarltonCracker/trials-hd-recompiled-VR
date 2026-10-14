#!/usr/bin/env python3
"""The sampling wrapper must close captured pipes when gdb finishes."""
import os
from pathlib import Path
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    gdb = temp / 'gdb'
    gdb.write_text('#!/usr/bin/env bash\nprintf "test|1\\tframe\\n" > "$SAMPLER_OUT"\n')
    gdb.chmod(0o755)
    env = dict(os.environ, PATH=str(temp) + os.pathsep + os.environ['PATH'])
    process = subprocess.Popen(
        ['bash', str(root / 'tools/profile/sample.sh'), '1', '1', '0.01', '', str(temp / 'samples')],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=3)
        assert process.returncode == 0, stderr.decode()
        assert b'1 thread samples' in stdout, stdout.decode()
    finally:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
print('PASS: profiler wrapper closes without lingering output pipes')
