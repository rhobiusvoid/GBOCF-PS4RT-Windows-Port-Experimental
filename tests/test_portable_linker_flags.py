"""Ensure the portable Linux linker configuration does not regress."""
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class PortableLinkerTests(unittest.TestCase):
    def test_build_script_syntax(self):
        subprocess.run(['bash', '-n', str(ROOT / 'build.sh')], check=True)

    def test_proven_transitive_dso_paths_present_only_in_portable_mode(self):
        source = (ROOT / 'build.sh').read_text()
        gate = 'if [[ ${PS4RT_PORTABLE_BUILD:-0} == 1 ]]; then'
        self.assertIn(gate, source)
        options = source.split(gate, 1)[1].split('\nfi', 1)[0]
        for flag in (
            '-L/usr/lib/x86_64-linux-gnu',
            '-Wl,-rpath-link,/opt/ps4rt-deps/lib',
            '-Wl,-rpath-link,/usr/lib/x86_64-linux-gnu',
            '-Wl,-rpath-link,/lib/x86_64-linux-gnu',
            '-print-file-name=libstdc++.so.6',
        ):
            self.assertIn(flag, options)
        self.assertIn('portable_libs[@]', source)


if __name__ == '__main__':
    unittest.main()
