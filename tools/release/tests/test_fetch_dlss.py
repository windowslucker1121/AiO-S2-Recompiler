# SPDX-License-Identifier: GPL-3.0-or-later
"""No network: fetch() with file:// URLs and synthetic bytes. Run: python -I tools/release/tests/test_fetch_dlss.py"""
import hashlib
import os
import pathlib
import subprocess
import sys
import tempfile

here = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(here.parent))
import fetch_dlss as f  # noqa: E402

with tempfile.TemporaryDirectory() as t:
    t = pathlib.Path(t)
    data = b'synthetic' * 1000
    src = t / 'src.bin'
    src.write_bytes(data)
    url = src.as_uri()
    sha = hashlib.sha256(data).hexdigest()
    dest = str(t / 'out' / 'x.dll')
    f.fetch(url, dest, sha, len(data))
    assert open(dest, 'rb').read() == data
    bad = str(t / 'out' / 'y.dll')
    for s, n in (('0' * 64, len(data)), (sha, len(data) + 1)):
        try:
            f.fetch(url, bad, s, n)
            raise SystemExit('mismatch accepted')
        except RuntimeError:
            pass
        assert not os.path.exists(bad) and not os.path.exists(bad + '.part')
    r = subprocess.run([sys.executable, '-I', str(here.parent / 'fetch_dlss.py'), '--out', str(t / 'o2')], capture_output=True)
    assert r.returncode == 2 and not (t / 'o2').exists(), 'ran without --accept-license'
print('ok')
