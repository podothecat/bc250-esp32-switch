"""Build release firmware and the power-log site for Cloudflare Pages.

    ~/.platformio/penv/Scripts/python scripts/publish_site.py --project NAME [--deploy]

Builds every env in TARGETS with a release FW_VERSION, stages
  dist/site/index.html             tools/power-log.html, pointed at this site
  dist/site/vendor/                esptool-js (the in-browser flasher)
  dist/site/fw/manifest.json       version + per-chip parts with sha256
  dist/site/fw/<version>/<env>/    firmware.bin, spiffs.bin
  dist/site/_headers               CORS for copies of the page opened from disk
then refuses to continue if any staged file contains a local path, the OS user
name, git identity or the GitHub owner. --deploy uploads with
`npx wrangler pages deploy` (run `npx wrangler login` once first).

The page reaches the site as https://NAME.pages.dev/, so pick a NAME that says
nothing about you.
"""
import argparse
import csv
import getpass
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SITE = ROOT / 'dist/site'
ESPTOOL_JS = 'esptool-js-0.5.4.mjs'

# env -> chip name as esptool-js reports it (loader.chip.CHIP_NAME).
TARGETS = {
    'esp32-wroom32': 'ESP32',
    'esp32-c3-devkitm-1': 'ESP32-C3',
}


def partition_offsets():
    rows = (r for r in csv.reader((ROOT / 'partitions.csv').read_text().splitlines())
            if r and not r[0].lstrip().startswith('#'))
    return {r[0].strip(): int(r[3].strip(), 0) for r in rows}


def pio(*args, version):
    cmd = [sys.executable, '-m', 'platformio', *args]
    if subprocess.run([sys.executable, '-c', 'import platformio'], capture_output=True).returncode:
        cmd = ['pio', *args]   # not running under PlatformIO's python
    env = dict(os.environ, BC250_FW_VERSION=version)
    subprocess.run(cmd, cwd=ROOT, env=env, check=True)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git_config(key):
    r = subprocess.run(['git', 'config', key], cwd=ROOT, capture_output=True, text=True)
    return r.stdout.strip()


def identity_needles():
    """Strings that would tie a published file back to this machine or account."""
    needles = {str(Path.home()), Path.home().as_posix(), str(ROOT), ROOT.as_posix(),
               getpass.getuser(), Path.home().name,
               git_config('user.name'), git_config('user.email')}
    remote = git_config('remote.origin.url')
    m = re.search(r'github\.com[:/]([^/]+)/', remote)
    if m:
        needles.add(m.group(1))
    return {n for n in needles if n and len(n) >= 3}


def scan_for_identity(root):
    needles = [n.lower().encode() for n in identity_needles()]
    hits = []
    for path in root.rglob('*'):
        if path.is_file():
            data = path.read_bytes().lower()
            hits += [(path.relative_to(root), n.decode()) for n in needles if n in data]
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--project', required=True, help='Cloudflare Pages project name')
    ap.add_argument('--version', help='firmware version (default: UTC timestamp)')
    ap.add_argument('--deploy', action='store_true', help='upload with wrangler when staged')
    args = ap.parse_args()

    if not re.fullmatch(r'[a-z0-9][a-z0-9-]{0,56}', args.project):
        sys.exit('project name: lowercase letters, digits and dashes only')
    version = args.version or datetime.now(timezone.utc).strftime('%Y.%m.%d-%H%M')
    base_url = f'https://{args.project}.pages.dev/'
    offsets = partition_offsets()

    if SITE.exists():
        shutil.rmtree(SITE)
    builds = []
    for env, chip in TARGETS.items():
        pio('run', '-e', env, version=version)
        pio('run', '-e', env, '-t', 'buildfs', version=version)
        out = SITE / 'fw' / version / env
        out.mkdir(parents=True)
        parts = []
        # App only + the portal UI. NVS (settings) and the bootloader are untouched.
        for name, partition in (('firmware.bin', 'app0'), ('spiffs.bin', 'spiffs')):
            src = ROOT / '.pio/build' / env / name
            shutil.copy2(src, out / name)
            parts.append({'path': f'fw/{version}/{env}/{name}', 'offset': offsets[partition],
                          'size': src.stat().st_size, 'sha256': sha256(src)})
        builds.append({'chip': chip, 'env': env, 'parts': parts})

    (SITE / 'fw/manifest.json').write_text(
        json.dumps({'version': version, 'builds': builds}, indent=2), encoding='utf-8')

    page = (ROOT / 'tools/power-log.html').read_text(encoding='utf-8')
    meta = f'<meta name="bc250-update-base" content="{base_url}" />'
    page = page.replace('<meta name="bc250-update-base" content="" />', meta, 1)
    assert meta in page, 'update-base meta tag missing from tools/power-log.html'
    (SITE / 'index.html').write_text(page, encoding='utf-8')

    (SITE / 'vendor').mkdir()
    for name in (ESPTOOL_JS, 'esptool-js-LICENSE.txt'):
        shutil.copy2(ROOT / 'tools/vendor' / name, SITE / 'vendor' / name)

    # A copy of the page opened from disk has origin "null", so the firmware and
    # the flasher module must be fetchable cross-origin. Nothing here is secret.
    (SITE / '_headers').write_text(
        '/fw/*\n  Access-Control-Allow-Origin: *\n  Cache-Control: no-cache\n'
        '/vendor/*\n  Access-Control-Allow-Origin: *\n'
        '/\n  Cache-Control: no-cache\n', encoding='utf-8')

    hits = scan_for_identity(SITE)
    if hits:
        for path, needle in hits:
            print(f'IDENTITY LEAK: {path} contains "{needle}"')
        sys.exit('refusing to publish; nothing was uploaded')

    print(f'\nstaged {SITE} (fw {version}) -> {base_url}')
    if args.deploy:
        subprocess.run(['npx', 'wrangler', 'pages', 'deploy', str(SITE),
                        '--project-name', args.project, '--branch', 'main',
                        '--commit-dirty=true'], cwd=ROOT, check=True, shell=os.name == 'nt')
    else:
        print('upload with: npx wrangler pages deploy dist/site '
              f'--project-name {args.project} --branch main')


if __name__ == '__main__':
    main()
