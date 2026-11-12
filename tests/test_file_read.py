import subprocess
import unittest
from paths import ROOT


class FileReadTest(unittest.TestCase):
    def test_reads_into_gpu_tracked_memory(self):
        exe = ROOT / 'out/file-read-test'
        if not exe.exists():
            self.skipTest('run build.sh --test first')
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS: tracked destination read/pread', result.stdout)
