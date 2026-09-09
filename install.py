#!/usr/bin/env python3
"""Install a private copy and user services; never requires root."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


def quote(value):
    return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"').replace('%', '%%') + '"'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--no-start', action='store_true', help='write files without calling systemctl')
    args = parser.parse_args()
    if sys.version_info < (3, 11):
        parser.error('Python 3.11+ required')
    os.umask(0o077)
    home = Path.home()
    destination = home / '.local/share/note4-quota-display'
    config = home / '.config/note4-quota-display'
    units = home / '.config/systemd/user'
    for path in (destination, config, units):
        path.mkdir(parents=True, exist_ok=True)
    for name in ('board_service.py', 'usage_board.py'):
        shutil.copyfile(Path(__file__).parent / 'host' / name, destination / name)
    env = config / 'environment'
    if not env.exists():
        codex = shutil.which('codex') or str(home / '.local/bin/codex')
        env.write_text('# NOTE4_DEVICE=/dev/serial/by-id/your-device\n'
                       '# NOTE4_SAVE_AUDIO=1\n'
                       '# HTTPS_PROXY=http://127.0.0.1:7897\n'
                       f'NOTE4_CODEX_BIN={quote(codex)}\n')
    for name, script, kind, extra in (
        ('note4-quota-display', 'board_service.py', 'simple', 'Restart=on-failure\nRestartSec=5\n'),
        ('note4-quota-refresh', 'usage_board.py', 'oneshot', 'TimeoutStartSec=65\n'),
    ):
        command = f'{quote(sys.executable)} {quote(destination / script)}'
        if kind == 'simple':
            command += ' run'
        (units / f'{name}.service').write_text(
            f'[Unit]\nDescription={name}\nStartLimitIntervalSec=0\n\n'
            f'[Service]\nType={kind}\nEnvironmentFile={quote(env)}\nExecStart={command}\n'
            f'{extra}UMask=0077\nNoNewPrivileges=true\n\n[Install]\nWantedBy=default.target\n')
    (units / 'note4-quota-refresh.timer').write_text(
        '[Unit]\nDescription=Refresh Note4 quota every two minutes\n\n'
        '[Timer]\nOnStartupSec=10\nOnUnitInactiveSec=120\nAccuracySec=5\n'
        'Unit=note4-quota-refresh.service\n\n[Install]\nWantedBy=timers.target\n')
    if not args.no_start:
        subprocess.run(['systemctl', '--user', 'daemon-reload'], check=True)
        subprocess.run(['systemctl', '--user', 'enable', 'note4-quota-display.service', 'note4-quota-refresh.timer'], check=True)
        subprocess.run(['systemctl', '--user', 'restart', 'note4-quota-display.service', 'note4-quota-refresh.timer'], check=True)
    print(f'Installed. Configuration: {env}')


if __name__ == '__main__':
    main()
