import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / 'host'))
import usage_board
import board_service


class QuotaTests(unittest.TestCase):
    def test_weekly_in_either_window(self):
        for key in ('primary', 'secondary'):
            data = {'rateLimits': {key: {'windowDurationMins': 10080,
                    'usedPercent': 42.5, 'resetsAt': time.time() + 86400}}}
            self.assertEqual(usage_board.parse_codex(data)['progress'], 43)

    def test_unknown_and_expired_are_not_zero(self):
        with self.assertRaises(ValueError):
            usage_board.parse_codex({'rateLimits': {}})
        with self.assertRaises(ValueError):
            usage_board.reset_time(1)
        for value in (float('nan'), -1, 101):
            with self.assertRaises(ValueError):
                usage_board.percent(value)

    def test_kimi_windows(self):
        row = {'used': 3, 'limit': 10, 'resetTime': '2099-01-01T00:00:00Z'}
        for duration, unit in ((5, 'TIME_UNIT_HOUR'), (300, 'TIME_UNIT_MINUTE')):
            result = usage_board.parse_kimi({'usage': row, 'limits': [
                {'window': {'duration': duration, 'timeUnit': unit}, 'detail': row}]})
            self.assertEqual((result['kimi_week'], result['kimi_5h']), (30, 30))

    def test_ambiguous_device_is_rejected(self):
        with patch.object(board_service, 'DEVICE', ''), patch.object(Path, 'glob', return_value=[Path('/a'), Path('/b')]):
            with self.assertRaises(OSError):
                board_service.connect()

    def test_isolated_install_preserves_config(self):
        with tempfile.TemporaryDirectory(prefix='note4 test ') as directory:
            env = dict(os.environ, HOME=directory)
            command = [sys.executable, str(PROJECT / 'install.py'), '--no-start']
            subprocess.run(command, env=env, check=True, capture_output=True)
            root = Path(directory)
            config = root / '.config/note4-quota-display/environment'
            config.write_text('NOTE4_DEVICE=/dev/test\n')
            subprocess.run(command, env=env, check=True, capture_output=True)
            self.assertEqual(config.read_text(), 'NOTE4_DEVICE=/dev/test\n')
            self.assertEqual(config.stat().st_mode & 0o777, 0o600)
            self.assertTrue((root / '.local/share/note4-quota-display/usage_board.py').exists())
            unit = (root / '.config/systemd/user/note4-quota-display.service').read_text()
            self.assertIn('" run\n', unit)
            self.assertNotIn('/home/johnny', unit)


if __name__ == '__main__':
    unittest.main()
