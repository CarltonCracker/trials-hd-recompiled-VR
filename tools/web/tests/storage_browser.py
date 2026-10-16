#!/usr/bin/env python3
"""Private real-game OPFS check: fresh import, reload, retained saves, second-tab lock.

Use .venv/bin/python; serves no files itself. Package and artifacts stay local.
"""
import argparse
import asyncio
import base64
import contextlib
from pathlib import Path
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cdp


async def drive(session, args):
    pump = asyncio.create_task(session.pump())
    try:
        target = await session.call('Target.createTarget', {'url': 'about:blank'})
        attached = await session.call('Target.attachToTarget', {'targetId': target['targetId'], 'flatten': True})
        page = session.page_session = attached['sessionId']
        for method in ('Runtime.enable', 'Page.enable', 'DOM.enable'):
            await session.call(method, session=page)
        await session.call('Target.setAutoAttach', {
            'autoAttach': True, 'waitForDebuggerOnStart': True, 'flatten': True}, session=page)
        await session.call('Emulation.setDeviceMetricsOverride', {
            'width': 1280, 'height': 720, 'deviceScaleFactor': 1, 'mobile': False}, session=page)

        async def evaluate(expression):
            result = await asyncio.wait_for(session.call('Runtime.evaluate', {
                'expression': expression, 'awaitPromise': True, 'returnByValue': True}, session=page), 20)
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

        async def reload():
            previous = await evaluate('window.trialsHd.started')
            await session.call('Page.reload', session=page)
            await until(f'!!window.trialsHd && window.trialsHd.started !== {previous}')

        async def choose(path, selector='#package-file'):
            document = await session.call('DOM.getDocument', session=page)
            node = await session.call('DOM.querySelector', {
                'nodeId': document['root']['nodeId'], 'selector': selector}, session=page)
            await session.call('DOM.setFileInputFiles', {
                'nodeId': node['nodeId'], 'files': [str(path.resolve())]}, session=page)

        await session.call('Page.navigate', {'url': args.url}, session=page)
        await until("!!document.getElementById('package-file')?.onchange")
        # A failed first import must not become a completed import.
        with tempfile.TemporaryDirectory() as temporary:
            invalid = Path(temporary) / 'invalid.stfs'
            invalid.write_bytes(b'not a package')
            await choose(invalid)
            await until('!!window.trialsHd?.storageError')
            await until('window.trialsHd.exitCode !== null')
            assert await evaluate('window.trialsHd.exitCode') == 1
            await reload()
            await until("!!document.getElementById('package-file')?.onchange")
        await choose(args.source, '#folder-files' if args.folder else '#package-file')
        await until('!!window.trialsHd?.filesReady')
        await until("document.querySelector('#log').textContent.includes('Keyboard controls:')")
        assert not await evaluate('!!window.trialsHd.storageError')
        assert await evaluate("(async () => { const root = await navigator.storage.getDirectory(); const game = await root.getDirectoryHandle('trials_hd'); const marker = await (await game.getFileHandle('import-ready')).getFile(); const home = await game.getDirectoryHandle('home'); const f = await home.getFileHandle('persistence-check', {create:true}); const w = await f.createWritable(); await w.write('save survives reload'); await w.close(); return (await marker.text()).trim(); })()") == '1'
        await reload()
        await until('!!window.trialsHd?.filesReady')
        assert await evaluate("(async () => { const root = await navigator.storage.getDirectory(); const game = await root.getDirectoryHandle('trials_hd'); const home = await game.getDirectoryHandle('home'); return (await (await home.getFileHandle('persistence-check')).getFile()).text(); })()") == 'save survives reload'
        assert await evaluate("document.getElementById('import-panel').hidden")
        assert not await evaluate("!!document.getElementById('package-file').onchange")
        await until("document.querySelector('#log').textContent.includes('guest fps:')")
        # A second tab must not get OPFS access handles while the first owns storage.
        second = await session.call('Target.createTarget', {'url': args.url})
        attached2 = await session.call('Target.attachToTarget', {'targetId': second['targetId'], 'flatten': True})
        second_page = attached2['sessionId']
        await session.call('Runtime.enable', session=second_page)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            result = await session.call('Runtime.evaluate', {
                'expression': 'window.trialsHd?.startupError || null', 'returnByValue': True}, session=second_page)
            error = result.get('result', {}).get('value')
            if error:
                assert 'another tab' in error, error
                break
            await asyncio.sleep(0.2)
        else:
            raise AssertionError('Second tab did not reject concurrent storage access')
        await session.call('Target.closeTarget', {'targetId': second['targetId']})
        # The untouched main menu is 72 guest draws/frame (the splash is not).
        await until("/draws\\/frame: 72\\.0/.test(document.querySelector('#log').textContent)")
        shot = await session.call('Page.captureScreenshot', {'format': 'png'}, session=page)
        args.console.with_suffix('.png').write_bytes(base64.b64decode(shot['data']))
        kind = 'folder import' if args.folder else 'package extraction'
        print(f'OPFS PASSED: failed import retry, real {kind}, reload without reimport, retained save, second-tab lock, rendered main menu')
        return 0
    finally:
        pump.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await pump


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('url')
    parser.add_argument('source', type=Path, help='Owned game package, or extracted assets with --folder')
    parser.add_argument('--folder', action='store_true', help='Exercise the directory picker instead of package extraction')
    parser.add_argument('--console', type=Path, default=Path('out/web-storage/console.log'))
    parser.add_argument('--chromium', default='chromium')
    args = parser.parse_args()
    cdp.drive = drive
    return asyncio.run(cdp.run(args))


if __name__ == '__main__':
    sys.exit(main())
