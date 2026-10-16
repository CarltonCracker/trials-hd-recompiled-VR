#!/usr/bin/env python3
"""Controller injection must fail loudly when the page hook is absent/broken."""
import sys
from pathlib import Path
import asyncio
import tempfile
from types import SimpleNamespace
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cdp


class FakeSession:
    page_session = 'page'
    def __init__(self, result):
        self.result = result
        self.calls = []
    async def call(self, method, params, session):
        self.calls.append((method, params, session))
        return self.result


class ControllerInjectionTest(unittest.IsolatedAsyncioTestCase):
    async def run_script(self, command, result):
        with tempfile.TemporaryDirectory() as folder:
            script = Path(folder) / 'script.txt'
            script.write_text(command + '\n')
            session = FakeSession(result)
            await cdp.run_headless_script(session, script, Path(folder))
            return session

    async def test_button_reaches_page_hook(self):
        session = await self.run_script('pad a 1', {})
        self.assertEqual(session.calls[0][1]['expression'], 'window.setSyntheticPadButton(0, 1);')

    async def test_missing_button_hook_is_error(self):
        with self.assertRaises(RuntimeError):
            await self.run_script('pad down 1', {'exceptionDetails': {'text': 'not defined'}})

    async def test_missing_axis_hook_is_error(self):
        with self.assertRaises(RuntimeError):
            await self.run_script('axis rt 32767', {'exceptionDetails': {'text': 'not defined'}})


class DriveSession:
    lines = 0
    matched = None

    def __init__(self, exit_code, exceptions=0):
        self.exit_code = exit_code
        self.exceptions = exceptions

    async def pump(self):
        await asyncio.Future()

    async def call(self, method, params=None, session=None):
        if method == 'Target.createTarget':
            return {'targetId': 'page'}
        if method == 'Target.attachToTarget':
            return {'sessionId': 'page'}
        if method == 'Runtime.evaluate':
            return {'result': {'value': self.exit_code}}
        return {}


class ExitStatusTest(unittest.IsolatedAsyncioTestCase):
    def args(self, script=None):
        return SimpleNamespace(until=None, viewport=None, init_js=None,
                               url='http://localhost/', seconds=2, script=script,
                               out_dir=Path('out'), eval=None, screenshot=None, console=Path('out/test.log'))

    async def test_clean_exit_can_end_script_early(self):
        with tempfile.TemporaryDirectory() as folder:
            script = Path(folder) / 'exit.txt'
            script.write_text('wait 60\n')
            self.assertEqual(await cdp.drive(DriveSession(0), self.args(script)), 0)

    async def test_page_failure_is_nonzero(self):
        self.assertEqual(await cdp.drive(DriveSession(1), self.args()), 1)

    async def test_uncaught_exception_is_nonzero(self):
        self.assertEqual(await cdp.drive(DriveSession(0, exceptions=1), self.args()), 1)


if __name__ == '__main__':
    unittest.main()
