#!/usr/bin/env python3
"""Local USB bridge for the existing Note4 JSON-lines firmware (stdlib only)."""
import argparse
import fcntl
import json
import logging
import os
from pathlib import Path
import re
import select
import termios
import time
import uuid
import wave

DEVICE = os.environ.get('NOTE4_DEVICE', '')
ROOT = Path(os.environ.get('NOTE4_STATE_DIR', str(Path.home() / '.local/state/note4-quota-display')))

def atomic(path, value):
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(value, ensure_ascii=False) + '\n')
    tmp.replace(path)

def connect():
    device = DEVICE
    if not device:
        candidates = sorted(Path('/dev/serial/by-id').glob('usb-Espressif_USB_JTAG_serial_debug_unit_*-if00'))
        if len(candidates) != 1:
            raise OSError('Set NOTE4_DEVICE: expected exactly one Espressif USB serial device')
        device = str(candidates[0])
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        a = termios.tcgetattr(fd)
        a[0] = a[1] = a[3] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[4] = a[5] = termios.B115200
        a[6][termios.VMIN] = a[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, a)
        return fd
    except Exception:
        os.close(fd)
        raise

def send(fd, data):
    deadline = time.monotonic() + 5
    while data:
        if time.monotonic() > deadline:
            raise TimeoutError('serial write timeout')
        if select.select([], [fd], [], .2)[1]:
            try:
                data = data[os.write(fd, data[:48]):]
                time.sleep(.03)
            except BlockingIOError:
                pass

def run():
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s')
    while True:
        fd = None
        try:
            fd = connect()
            buffer = b''
            recording = None
            pending = None
            acknowledged = False
            last_info = 0
            info_at = time.monotonic()
            logging.info('USB connected')
            send(fd, b'\nINFO\n')
            while True:
                now = time.monotonic()
                if recording is not None and now - recording[3] > 30:
                    raise TimeoutError('incomplete audio')
                if now - info_at > 90:
                    raise TimeoutError('firmware INFO response missing')
                if now - last_info > 30 and recording is None:
                    send(fd, b'INFO\n')
                    last_info = now
                if acknowledged and pending is None and recording is None:
                    queue = sorted((ROOT / 'queue').glob('*.json'))
                    if queue:
                        pending = queue[0]
                        pending_data = pending.read_bytes()
                        send(fd, pending_data)
                        sent_at = now
                if pending is not None and now - sent_at > 60:
                    raise TimeoutError('JSON acknowledgement missing')
                if not select.select([fd], [], [], .2)[0]:
                    continue
                chunk = os.read(fd, 8192)
                if not chunk:
                    raise OSError('USB disconnected')
                buffer += chunk
                while buffer:
                    if recording is not None:
                        size, rate, target, started = recording
                        if len(buffer) < size:
                            if time.monotonic() - started > 30:
                                raise TimeoutError('incomplete audio')
                            break
                        path = ROOT / 'audio' / (str(time.time_ns()) + '-' + target + '.wav')
                        if os.environ.get('NOTE4_SAVE_AUDIO') == '1':
                            with wave.open(str(path), 'wb') as wav:
                                wav.setnchannels(1)
                                wav.setsampwidth(2)
                                wav.setframerate(rate)
                                wav.writeframes(buffer[:size])
                            logging.info('Audio saved locally: %s', path.name)
                        buffer = buffer[size:]
                        recording = None
                        continue
                    if b'\n' not in buffer:
                        if len(buffer) > 16384:
                            raise ValueError('oversized serial line')
                        break
                    line, buffer = buffer.split(b'\n', 1)
                    line = line.decode('utf-8', errors='replace').strip()
                    match = re.fullmatch(r'@AUDIO (\d+) (\d+) (codex|kimi)', line)
                    if match:
                        size, rate = map(int, match.groups()[:2])
                        if not 0 < size <= 640000 or rate != 16000 or size % 2:
                            raise ValueError('invalid audio header')
                        recording = (size, rate, match[3], time.monotonic())
                    elif line.startswith('@INFO '):
                        acknowledged = True
                        info_at = time.monotonic()
                        atomic(ROOT / 'health.json', {'connected': True, 'checked_at': time.time(), 'info': line})
                        logging.info('%s', line)
                    elif line == 'OK' and pending is not None:
                        if pending.read_bytes() == pending_data:
                            pending.unlink()
                        pending = None
                        logging.info('Display update acknowledged')
                    elif line.startswith('@TARGET '):
                        atomic(ROOT / 'target.json', {'target': line.split()[-1]})
                    elif line.startswith('ERR'):
                        raise ValueError(line)
        except (OSError, ValueError, TimeoutError) as exc:
            atomic(ROOT / 'health.json', {'connected': False, 'checked_at': time.time(), 'error': str(exc)})
            logging.warning('%s; retry in 5s', exc)
        finally:
            if fd is not None:
                os.close(fd)
        time.sleep(5)

def main():
    os.umask(0o077)
    for name in ('queue', 'audio'):
        (ROOT / name).mkdir(parents=True, exist_ok=True)
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=['run', 'status', 'push'])
    parser.add_argument('json', nargs='?')
    args = parser.parse_args()
    if args.command == 'run':
        run()
    elif args.command == 'status':
        print((ROOT / 'health.json').read_text() if (ROOT / 'health.json').exists() else 'Service has not connected yet')
    else:
        payload = json.loads(args.json)
        if not isinstance(payload, dict) or payload.get('type') not in ('state', 'usage'):
            parser.error('JSON type must be state or usage')
        if len(json.dumps(payload, ensure_ascii=False).encode()) > 12000:
            parser.error('payload too large')
        path = ROOT / 'queue' / (str(time.time_ns()) + '-' + uuid.uuid4().hex + '.json')
        atomic(path, payload)
        print(path)

if __name__ == '__main__':
    main()
