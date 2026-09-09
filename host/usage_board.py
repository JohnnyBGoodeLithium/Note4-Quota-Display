#!/usr/bin/env python3
"""Fetch account quota only; queue the existing firmware's usage payload."""
import asyncio
from datetime import datetime
import json
import logging
import math
import os
import traceback
from pathlib import Path
import urllib.request
from board_service import ROOT, atomic


async def codex_limits():
    proc = await asyncio.create_subprocess_exec(
        os.environ.get('NOTE4_CODEX_BIN', 'codex'), 'app-server',
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL, limit=2**20)
    async def send(data):
        proc.stdin.write((json.dumps(data) + '\n').encode())
        await proc.stdin.drain()
    async def receive(wanted):
        while True:
            line = await proc.stdout.readline()
            if not line:
                raise RuntimeError('Codex app-server exited')
            data = json.loads(line)
            if data.get('id') == wanted:
                if 'error' in data:
                    raise RuntimeError('Codex quota request failed')
                return data['result']
    try:
        async with asyncio.timeout(35):
            await send({'id': 1, 'method': 'initialize', 'params': {
                'clientInfo': {'name': 'note4_quota', 'version': '1.0'}}})
            await receive(1)
            await send({'method': 'initialized'})
            await send({'id': 2, 'method': 'account/rateLimits/read'})
            return await receive(2)
    finally:
        if proc.returncode is None:
            proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), 5)
            except TimeoutError:
                proc.kill()
                await proc.wait()


def kimi_limits():
    credentials = json.loads((Path.home() / '.kimi-code/credentials/kimi-code.json').read_text())
    # Re-read the managed client's token each run; never copy or log credentials.
    request = urllib.request.Request('https://api.kimi.com/coding/v1/usages', headers={
        'Authorization': 'Bearer ' + credentials['access_token'], 'Accept': 'application/json'})
    with urllib.request.urlopen(request, timeout=20) as response:
        return json.load(response)


def reset_time(value):
    if isinstance(value, (int, float)):
        dt = datetime.fromtimestamp(value).astimezone()
    else:
        dt = datetime.fromisoformat(value.replace('Z', '+00:00')).astimezone()
    if dt.timestamp() <= datetime.now().timestamp():
        raise ValueError('quota reset already passed')
    return dt.strftime('%m-%d %H:%M')


def percent(value):
    number = float(value)
    if not math.isfinite(number) or not 0 <= number <= 100:
        raise ValueError('invalid quota percentage')
    return math.floor(number + .5)


def parse_codex(data):
    bucket = data.get('rateLimitsByLimitId', {}).get('codex') or data.get('rateLimits', {})
    weekly = next((bucket[key] for key in ('primary', 'secondary')
                   if bucket.get(key) and bucket[key].get('windowDurationMins') == 10080), None)
    if weekly is None:
        raise ValueError('Codex weekly quota missing')
    return {'progress': percent(weekly['usedPercent']), 'codex_reset': reset_time(weekly['resetsAt'])}


def parse_kimi(data):
    def row(detail):
        limit = float(detail['limit'])
        if limit <= 0:
            raise ValueError('invalid Kimi quota limit')
        return percent(float(detail['used']) / limit * 100), reset_time(detail['resetTime'])
    weekly, week_reset = row(data['usage'])
    five = None
    units = {'TIME_UNIT_MINUTE': 1, 'TIME_UNIT_HOUR': 60}
    for item in data.get('limits', []):
        window = item.get('window', {})
        if float(window.get('duration', 0)) * units.get(window.get('timeUnit'), 0) == 300:
            five = row(item['detail'])
            break
    if five is None:
        raise ValueError('Kimi five-hour quota missing')
    return {'kimi_week': weekly, 'kimi_week_reset': week_reset,
            'kimi_5h': five[0], 'kimi_5h_reset': five[1]}


async def main():
    os.umask(0o077)
    (ROOT / 'queue').mkdir(parents=True, exist_ok=True)
    payload = {'type': 'usage', 'progress': -1, 'codex_reset': '--',
               'kimi_week': -1, 'kimi_week_reset': '--', 'kimi_5h': -1, 'kimi_5h_reset': '--'}
    results = await asyncio.gather(codex_limits(), asyncio.to_thread(kimi_limits), return_exceptions=True)
    successes = 0
    errors = {}
    for name, result, parser in zip(('codex', 'kimi'), results, (parse_codex, parse_kimi)):
        try:
            if isinstance(result, BaseException):
                raise result
            payload.update(parser(result))
            successes += 1
        except Exception as exc:
            # Exception text could include server content; only record the type.
            errors[name] = type(exc).__name__
            logging.warning('%s: %s at %s', name, type(exc).__name__,
                            [(Path(f.filename).name, f.lineno, f.name)
                             for f in traceback.extract_tb(exc.__traceback__)])
    payload['sync_status'] = ('failed', 'partial', 'ok')[successes]
    payload['sync_at'] = datetime.now().strftime('%H:%M')
    atomic(ROOT / 'usage.json', {'payload': payload, 'errors': errors,
                               'checked_at': datetime.now().astimezone().isoformat(),
                               'percent_meaning': 'used'})
    # A stable filename coalesces updates when the board is unplugged.
    atomic(ROOT / 'queue/usage.json', payload)
    print(json.dumps({'payload': payload, 'errors': errors}, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    asyncio.run(main())
