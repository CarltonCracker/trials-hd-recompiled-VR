#!/usr/bin/env python3
"""Record window.spikeResult and console from a page in an existing CDP browser.

Usage: .venv/bin/python tools/web/spikes/record.py http://127.0.0.1:9222 out/result.json
Start the browser on the spike URL first. Output stays in out/ (never commit logs).
"""

import asyncio
import json
from pathlib import Path
import sys
from urllib.request import urlopen

from websockets.asyncio.client import connect


async def record(endpoint, output):
    with urlopen(endpoint.rstrip('/') + '/json', timeout=10) as response:
        pages = json.load(response)
    page = next(page for page in pages if page['type'] == 'page')
    async with connect(page['webSocketDebuggerUrl'], max_size=8 * 1024 * 1024) as ws:
        await ws.send(json.dumps({'id': 1, 'method': 'Runtime.enable'}))
        await ws.send(json.dumps({'id': 2, 'method': 'Runtime.evaluate', 'params': {
            'expression': 'window.spikeResult', 'awaitPromise': True, 'returnByValue': True}}))
        with output.with_suffix('.console.log').open('w') as console:
            while True:
                message = json.loads(await asyncio.wait_for(ws.recv(), 60))
                if message.get('method') == 'Runtime.consoleAPICalled':
                    text = ' '.join(str(arg.get('value', arg.get('description', '')))
                                    for arg in message['params']['args'])
                    print(text, file=console, flush=True)
                if message.get('id') == 2:
                    if 'error' in message or 'exceptionDetails' in message.get('result', {}):
                        raise RuntimeError(message)
                    result = message['result']['result']
                    if 'value' not in result:
                        raise RuntimeError('Page has no resolved window.spikeResult: ' + str(result))
                    output.write_text(json.dumps(result['value'], indent=2) + '\n')
                    print(output.read_text())
                    return


if __name__ == '__main__':
    asyncio.run(record(sys.argv[1], Path(sys.argv[2])))
