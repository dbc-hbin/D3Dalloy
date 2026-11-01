#!/usr/bin/env python3
"""Consumer-visible ARM64 preflight rejection and evidence-boundary tests."""
from __future__ import annotations

import hashlib
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))
SPEC = importlib.util.spec_from_file_location('preflight_owned_arm64', Path(__file__).with_name('preflight-owned-arm64.py'))
assert SPEC and SPEC.loader
preflight = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(preflight)


class PreflightTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.records = {}
        self.owned_records = {}
        # Architecture/provenance consumer tests receive a verified stage inventory.
        # The real stage verifier is separately exercised by the read-only stage smoke.
        self.owner_patch = mock.patch.object(preflight, 'verified_owned_artifacts', return_value=self.owned_records)
        self.owner_patch.start()
        self.addCleanup(self.owner_patch.stop)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def artifact(self, relative: str, payload: bytes) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        self.records[relative] = {'source_url': 'https://github.com/wine-mirror/wine/tree/' + '1' * 40,
                                  'revision': '1' * 40, 'sha256': hashlib.sha256(payload).hexdigest()}
        return path

    def native(self, relative: str, cpu: int = 0x100000C) -> Path:
        return self.artifact(relative, struct.pack('<IIIIIIII', 0xfeedfacf, cpu, 0, 2, 0, 0, 0, 0))

    def pe(self, relative: str, machine: int = 0xAA64) -> Path:
        data = bytearray(64 + 24 + 112)
        data[:2] = b'MZ'
        struct.pack_into('<I', data, 60, 64)
        data[64:68] = b'PE\0\0'
        struct.pack_into('<HH', data, 68, machine, 0)
        struct.pack_into('<H', data, 84, 112)
        struct.pack_into('<H', data, 88, 0x20b)
        return self.artifact(relative, bytes(data))

    def plain_candidate(self) -> None:
        for relative in ('bin/wine', 'bin/wineserver', 'lib/wine/aarch64-unix/ntdll.so',
                         'lib/wine/aarch64-unix/winemetal.so'):
            self.native(relative)
        self.pe('lib/wine/aarch64-windows/ntdll.dll')
        self.pe('lib/wine/aarch64-windows/winemetal.dll')
        for relative in ('lib/wine/aarch64-unix/winemetal.so', 'lib/wine/aarch64-windows/winemetal.dll'):
            self.owned_records[relative] = {'sha256': self.records.pop(relative)['sha256'],
                                           'provenance_type': 'verified-owned-stage',
                                           'source': {'d3dalloy_install': 'isolated owned contract fixture'}}

    def audit(self, abi: str = 'arm64', guest: str = 'arm64') -> dict:
        return preflight.audit_candidate(self.root, abi, guest, {'schema': 1, 'artifacts': self.records})

    def test_verified_owned_candidate_is_static_eligible_not_execution_proof(self) -> None:
        self.plain_candidate()
        result = self.audit()
        self.assertTrue(result['static_eligible'])
        self.assertEqual(result['execution_status'], 'BLOCKED')
        self.assertFalse(result['strict_no_rosetta_proven'])


    def test_x64_only_wine_rejected_even_with_arm64_server(self) -> None:
        self.plain_candidate()
        self.native('bin/wine', 0x1000007)
        result = self.audit()
        self.assertFalse(result['static_eligible'])
        self.assertIn('ROSETTA_REQUIRED: no arm64 Mach-O slice: bin/wine', result['blockers'])

    def test_extra_native_dependency_is_audited(self) -> None:
        self.plain_candidate()
        self.native('lib/wine/aarch64-unix/vendor.so', 0x1000007)
        self.assertIn('ROSETTA_REQUIRED: no arm64 Mach-O slice: lib/wine/aarch64-unix/vendor.so', self.audit()['blockers'])

    def test_unused_intel_unix_sibling_does_not_invalidate_arm64_selection(self) -> None:
        self.plain_candidate()
        self.native('lib/wine/x86_64-unix/ntdll.so', 0x1000007)
        result = self.audit()
        self.assertTrue(result['static_eligible'])
        self.assertFalse(result['native_dependency_closure_verified'])

    def test_plain_arm64_core_cannot_service_x64_guest(self) -> None:
        self.plain_candidate()
        result = self.audit('arm64', 'x64')
        self.assertIn('WINDOWS_ABI_MISMATCH: x64 guest cannot use arm64 core DLLs', result['blockers'])
        self.assertTrue(any('PE_ABI_MISMATCH' in reason for reason in result['blockers']))

    def test_fex_cpu_module_required_for_x64_not_native_arm64(self) -> None:
        self.plain_candidate()
        result = self.audit('arm64', 'x64')
        self.assertTrue(any('missing FEX x64 PE module' in reason for reason in result['blockers']))
        self.assertFalse(any('FEX' in reason for reason in self.audit()['blockers']))

    def test_hash_drift_invalidates_provenance(self) -> None:
        self.plain_candidate()
        self.records['bin/wine']['sha256'] = '0' * 64
        self.assertIn('runtime provenance hash mismatch: bin/wine', self.audit()['blockers'])

    def test_owned_hash_drift_rejected(self) -> None:
        self.plain_candidate()
        path = self.root / 'lib/wine/aarch64-unix/winemetal.so'
        path.write_bytes(path.read_bytes() + b'changed')
        self.assertIn('owned stage artifact hash mismatch: lib/wine/aarch64-unix/winemetal.so', self.audit()['blockers'])

    def test_official_url_cannot_self_label_repo_owned_artifact(self) -> None:
        self.plain_candidate()
        relative = 'lib/wine/aarch64-unix/winemetal.so'
        del self.owned_records[relative]
        self.records[relative] = self.records['bin/wine']
        self.assertIn('unknown repo-owned stage provenance: ' + relative, self.audit()['blockers'])

    def test_unknown_runtime_provenance_refused(self) -> None:
        self.plain_candidate()
        del self.records['bin/wineserver']
        self.assertIn('unknown runtime provenance: bin/wineserver', self.audit()['blockers'])

    def test_mutable_revision_not_a_pin(self) -> None:
        self.plain_candidate()
        self.records['bin/wine']['revision'] = 'main'
        self.assertIn('runtime git revision must be a full commit: bin/wine', self.audit()['blockers'])

    def test_source_host_spoof_and_insecure_urls_refused(self) -> None:
        self.plain_candidate()
        for url in ('https://github.com.evil.invalid/wine-mirror/wine/tree/a',
                    'http://github.com/wine-mirror/wine/tree/a',
                    'https://evil.invalid/FEX.dll', 'https://user@github.com/wine-mirror/wine/tree/a'):
            with self.subTest(url=url):
                self.records['bin/wine']['source_url'] = url
                self.assertIn('non-official runtime source: bin/wine', self.audit()['blockers'])

    def test_native_symlink_escape_refused(self) -> None:
        self.plain_candidate()
        with tempfile.TemporaryDirectory() as outside:
            destination = Path(outside) / 'winemetal.so'
            original = self.root / 'lib/wine/aarch64-unix/winemetal.so'
            destination.write_bytes(original.read_bytes())
            original.unlink()
            original.symlink_to(destination)
            self.assertTrue(any('symlink escapes audited root' in reason for reason in self.audit()['blockers']))

    def test_cli_cannot_publish_evidence_inside_runtime(self) -> None:
        destination = self.root / 'evidence'
        result = subprocess.run([sys.executable, str(Path(__file__).with_name('preflight-owned-arm64.py')),
                                 '--wine-runtime', str(self.root), '--out', str(destination)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertFalse(destination.exists())


if __name__ == '__main__':
    unittest.main()
