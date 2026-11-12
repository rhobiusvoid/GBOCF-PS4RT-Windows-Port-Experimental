import subprocess
import unittest
from paths import ROOT

EXE=ROOT/'out/movie-order-test'


class MovieOrderTest(unittest.TestCase):
    def test_next_frame_request_waits_for_the_queued_upload(self):
        if not EXE.exists():
            self.skipTest('run build.sh --test first')
        result=subprocess.run([str(EXE)],capture_output=True,text=True,timeout=60)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('movie order ok',result.stdout)
