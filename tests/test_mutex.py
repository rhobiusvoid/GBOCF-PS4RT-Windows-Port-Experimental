import subprocess
import unittest
from pathlib import Path
from paths import ROOT

EXE=ROOT/'out/mutex-test'


class MutexTest(unittest.TestCase):
    def test_destroyed_mutex_is_rejected_not_fatal(self):
        if not EXE.exists():
            self.skipTest('run build.sh --test first')
        result=subprocess.run([str(EXE)],capture_output=True,text=True,timeout=60)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('mutex ok',result.stdout)
