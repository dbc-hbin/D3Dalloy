#!/usr/bin/env python3
"""Check that an interrupted guest retains observed execution evidence."""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))
SPEC = importlib.util.spec_from_file_location('owned_wine_arm64_runner', Path(__file__).with_name('run-owned-wine-arm64.py'))
assert SPEC and SPEC.loader
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class RunnerTests(unittest.TestCase):
    def test_smoke_requires_all_ordered_exact_frames_and_final_success(self) -> None:
        frames = [f"OWNED_D3D12_SMOKE_FRAME frame={index} readback=ok present=skipped" for index in range(8)]
        accepted = "\n".join([*frames, "OWNED_D3D12_SMOKE_PASS"])
        self.assertEqual(runner.smoke_evidence(accepted), {"frames": list(range(8)), "exact_readback": True})
        rejected = ("\n".join(frames), accepted.replace(frames[3], ""),
                    accepted.replace(frames[3], frames[4]), accepted + "\nOWNED_D3D12_SMOKE_FAIL step=readback",
                    accepted + "\nOWNED_D3D12_SMOKE_PASS")
        for text in rejected:
            with self.subTest(output=text):
                self.assertFalse(runner.smoke_evidence(text)["exact_readback"])

    def test_timeout_retains_observations_without_claiming_acceptance(self) -> None:
        with tempfile.TemporaryDirectory(dir=runner.ROOT / 'build/local') as temporary:
            base = Path(temporary)
            runtime = base / 'runtime'
            runtime.mkdir()
            for suite, entered in (("abi", False), ("abi", True), ("smoke", False), ("smoke", True)):
                with self.subTest(suite=suite, guest_entered=entered):
                    output = base / (suite + str(entered))
                    server = mock.MagicMock(pid=21)
                    guest = mock.MagicMock(pid=22, returncode=-9)
                    guest.__enter__.return_value = guest
                    guest.wait.side_effect = [subprocess.TimeoutExpired(['wine'], 1), -9]

                    def spawn(command, **kwargs):
                        if command[0].endswith('wineserver'):
                            return server
                        if entered:
                            kwargs['stdout'].write('{"guest_entry":true,"pointer_bits":64}\n' if suite == "abi" else
                                                   'OWNED_D3D12_SMOKE_FAIL step=initialization\n')
                        images = Path(kwargs['env']['DXMT_ARM64_EVIDENCE'])
                        (images / '22.images').write_text('PROCESS\t22\t0\t/native/wine\nIMAGE\t22\t16777228\t/native/ntdll.so\n')
                        return guest

                    arguments = ['runner', '--runtime', str(runtime), '--fixture', str(base),
                                 '--output', str(output), '--timeout', '1', '--suite', suite]
                    with mock.patch.object(sys, 'argv', arguments), \
                         mock.patch.object(runner, 'inspect_pe', return_value={'windows_abi': 'arm64'}), \
                         mock.patch.object(runner.subprocess, 'Popen', side_effect=spawn), \
                         mock.patch.object(runner.subprocess, 'run'), \
                         mock.patch.object(runner.subprocess, 'check_output', return_value='27.0\n'), \
                         contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(runner.main(), 1)
                    result = json.loads((output / 'result.json').read_text())
                    self.assertTrue(result['runtime_executed'])
                    self.assertEqual(result['guest_executed'], entered)
                    self.assertTrue(result['timed_out'])
                    self.assertEqual(result['status'], 'blocked')
                    self.assertFalse(result['strict_no_rosetta_proven'])
                    self.assertEqual(result['processes'][0]['pid'], 22)
                    self.assertEqual(result['loaded_native'][0]['selected_cpu_type'], 0x100000c)
                    self.assertTrue(result['prefix_cleaned'])
                    guest.kill.assert_called_once_with()
                    guest.wait.assert_has_calls([mock.call(timeout=1), mock.call()])


if __name__ == '__main__':
    unittest.main()
