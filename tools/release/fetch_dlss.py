# SPDX-License-Identifier: GPL-3.0-or-later
"""Downloads NVIDIA's nvngx_dlss.dll for the library's "Download DLSS" button.

    python -I tools/scripts/fetch_dlss.py --accept-license --out <exe dir>/upscalers/streamline

The package does not carry this file: it is under the NVIDIA RTX SDKs LICENSE (https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt),
whose redistribution terms do not fit a GPL-3.0 package (LEGAL.md, "DLSS"). The library shows that licence and runs this script only
after the user clicked OK; the script refuses to run without --accept-license.
Pin: DLSS v310.9.16 from NVIDIA's own repository. It is byte-identical to bin/x64/nvngx_dlss.dll of the Streamline SDK v2.14.16
release (the version of runtime/third_party/nvidia_streamline and of the sl.*.dll the package ships), so the pin matches the headers.
Output for the library, one per line on stdout:  @progress download <done> <total> <text>  /  @error <text>
Exit codes: 0 present and verified, 1 error, 2 licence not accepted.
"""
import argparse
import hashlib
import os
import sys
import urllib.request

URL = 'https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlss.dll'
SHA256 = 'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e'
SIZE = 58956912
NAME = 'nvngx_dlss.dll'


def say(kind, *parts):
    print('@' + kind + ' ' + ' '.join(str(p) for p in parts), flush=True)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def fetch(url, dest, sha, size):
    """Downloads url to dest (via dest.part); installs it only when size and sha256 match. Raises RuntimeError otherwise."""
    part = dest + '.part'
    try:
        os.makedirs(os.path.dirname(dest) or '.', exist_ok=True)
        if os.path.isfile(dest) and os.path.getsize(dest) == size and sha256_of(dest) == sha:
            say('progress', 'download', size, size, NAME + ' is already here and matches NVIDIA\'s release.')
            return
        say('progress', 'download', 0, size, 'Downloading %s from NVIDIA...' % NAME)
        h = hashlib.sha256()
        done = 0
        req = urllib.request.Request(url, headers={'User-Agent': 'RecompLibrary'})
        with urllib.request.urlopen(req, timeout=30) as r, open(part, 'wb') as out:
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                out.write(b)
                h.update(b)
                done += len(b)
                if done > size + (1 << 20):
                    raise RuntimeError('the download is larger than NVIDIA\'s release; stopped')
                say('progress', 'download', done, size, 'Downloading %s from NVIDIA...' % NAME)
        if done != size or h.hexdigest() != sha:
            raise RuntimeError('the downloaded file does not match the pinned NVIDIA release (%d bytes, sha256 %s); it was discarded'
                               % (done, h.hexdigest()))
        os.replace(part, dest)
        say('progress', 'download', size, size, NAME + ' downloaded and verified.')
    finally:
        try:
            os.remove(part)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True, help='target folder (<exe dir>/upscalers/streamline)')
    ap.add_argument('--accept-license', action='store_true',
                    help='the user has seen and accepted the NVIDIA RTX SDKs LICENSE (the library passes this after its dialog)')
    a = ap.parse_args()
    if not a.accept_license:
        say('error', 'The NVIDIA RTX SDKs LICENSE has not been accepted; nothing was downloaded.')
        return 2
    try:
        fetch(URL, os.path.join(a.out, NAME), SHA256, SIZE)
        return 0
    except Exception as e:  # noqa: BLE001 - reported to the library in one line
        say('error', 'Could not get %s: %s' % (NAME, e))
        return 1


if __name__ == '__main__':
    sys.exit(main())
