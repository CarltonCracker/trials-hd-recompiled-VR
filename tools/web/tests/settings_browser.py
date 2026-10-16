#!/usr/bin/env python3
"""Real browser UI check: mouse rebind/save, config reload, resize and fullscreen.

Run with .venv Python against a local build URL with web_assets_url=/assets and
log_guest_fps=true. Keep --console under out/; private captures go beside it.
"""
import argparse
import asyncio
import base64
import contextlib
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cdp


async def drive(session, args):
    pump = asyncio.create_task(session.pump())
    try:
        target = await session.call('Target.createTarget', {'url': 'about:blank'})
        attached = await session.call('Target.attachToTarget', {'targetId': target['targetId'], 'flatten': True})
        page = session.page_session = attached['sessionId']
        for method in ('Runtime.enable', 'Page.enable'):
            await session.call(method, session=page)
        await session.call('Target.setAutoAttach', {
            'autoAttach': True, 'waitForDebuggerOnStart': True, 'flatten': True}, session=page)

        async def evaluate(expression):
            result = await asyncio.wait_for(session.call('Runtime.evaluate', {
                'expression': expression, 'awaitPromise': True, 'returnByValue': True}, session=page), 15)
            if 'exceptionDetails' in result:
                raise AssertionError(result['exceptionDetails'])
            return result.get('result', {}).get('value')

        async def until(expression, seconds=90):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if await evaluate(expression):
                    return
                await asyncio.sleep(0.2)
            raise AssertionError(f'Timed out: {expression}')

        async def key(code, text):
            await session.call('Input.dispatchKeyEvent', {
                'type': 'rawKeyDown', 'code': code, 'key': text}, session=page)
            await asyncio.sleep(0.1)
            await session.call('Input.dispatchKeyEvent', {
                'type': 'keyUp', 'code': code, 'key': text}, session=page)
            await asyncio.sleep(0.3)

        async def click(x, y, count=1):
            await session.call('Input.dispatchMouseEvent', {
                'type': 'mouseMoved', 'x': x, 'y': y}, session=page)
            await session.call('Input.dispatchMouseEvent', {
                'type': 'mousePressed', 'button': 'left', 'buttons': 1,
                'clickCount': count, 'x': x, 'y': y}, session=page)
            await asyncio.sleep(0.08)
            await session.call('Input.dispatchMouseEvent', {
                'type': 'mouseReleased', 'button': 'left', 'buttons': 0,
                'clickCount': count, 'x': x, 'y': y}, session=page)
            await asyncio.sleep(0.2)

        async def viewport(width, height, dpr=1):
            await session.call('Emulation.setDeviceMetricsOverride', {
                'width': width, 'height': height, 'deviceScaleFactor': dpr, 'mobile': False}, session=page)

        async def screenshot(name):
            shot = await session.call('Page.captureScreenshot', {'format': 'png'}, session=page)
            args.console.with_name(name + '.png').write_bytes(base64.b64decode(shot['data']))

        await viewport(1280, 720)
        await session.call('Page.navigate', {'url': args.url}, session=page)
        menu = r"/draws\/frame: 72\.0/.test(document.querySelector('#log')?.textContent || '')"
        await until(menu)
        await key('F1', 'F1')
        await asyncio.sleep(0.5)
        # Throttle's primary slot, at the fixed 1280x720 test viewport.
        await click(620, 121)
        await key('KeyT', 't')
        await screenshot('binding-changed')
        await session.call('Input.dispatchMouseEvent', {
            'type': 'mouseMoved', 'x': 800, 'y': 600}, session=page)
        await session.call('Input.dispatchMouseEvent', {
            'type': 'mouseWheel', 'x': 800, 'y': 600, 'deltaX': 0, 'deltaY': 3000}, session=page)
        await asyncio.sleep(0.5)
        await screenshot('settings-footer')
        await click(267, 672)
        await until("document.querySelector('#log').textContent.includes('Graphics settings saved to /opfs/trials_hd/trials_hd.toml')", 10)
        config = await evaluate("(async () => { const root = await navigator.storage.getDirectory(); const game = await root.getDirectoryHandle('trials_hd'); return (await (await game.getFileHandle('trials_hd.toml')).getFile()).text(); })()")
        line = next(line for line in config.splitlines() if line.startswith('keybind_right_trigger'))
        assert 'T,W' in line, line
        await screenshot('settings-saved')
        previous = await evaluate('window.trialsHd.started')
        await session.call('Page.reload', session=page)
        await until(f'!!window.trialsHd && window.trialsHd.started !== {previous}')
        await until(menu)
        await key('F1', 'F1')
        await screenshot('binding-after-reload')
        await key('F1', 'F1')
        # Resize after OffscreenCanvas transfer, then move to a high-DPI viewport.
        await viewport(960, 540, 2)
        await until('window.trialsHd.input.u32[4240/4] === 1920 && window.trialsHd.input.u32[4244/4] === 1080')
        await asyncio.sleep(1)
        await screenshot('resized-dpr2')
        # A trusted double click is required for the browser fullscreen API.
        await click(300, 300)
        await click(300, 300, 2)
        await until("document.fullscreenElement === document.getElementById('canvas')", 10)
        assert not await evaluate('!!window.trialsHd.storageError')
        assert not session.exceptions, f'{session.exceptions} uncaught browser errors'
        print('SETTINGS PASSED: mouse rebind/save to OPFS, reload, live resize/DPR2, trusted fullscreen')
        return 0
    finally:
        pump.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await pump


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('url')
    parser.add_argument('--console', type=Path, default=Path('out/web-acceptance/settings-interactive.log'))
    parser.add_argument('--chromium', default='chromium')
    args = parser.parse_args()
    cdp.drive = drive
    return asyncio.run(cdp.run(args))


if __name__ == '__main__':
    sys.exit(main())
