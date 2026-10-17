"""Host checks for the analyzer inside tools/power-log.html.
Run: python3 tests/power_log_host_test.py (requires node).
The page's serial/UI half needs a browser and the real board.
"""
from pathlib import Path
import json
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
page = (ROOT / 'tools/power-log.html').read_text(encoding='utf-8')
analyzer = re.search(r'<script id="analyzer">(.*?)</script>', page, re.S).group(1)

# The page cannot read board.h at runtime, so its copy of the thresholds must match.
board = (ROOT / 'include/board.h').read_text(encoding='utf-8')
for name in ('SENSE_LOW_MV', 'SENSE_HIGH_MV'):
    want = re.search(r'\b%s\s*=\s*(\d+)' % name, board).group(1)
    got = re.search(r'\b%s\s*=\s*(\d+)' % name, analyzer).group(1)
    assert want == got, f'{name}: board.h {want} != power-log.html {got}'


def hb(state, board, mv, raw):
    # Same shape as the firmware's [HB  ] printf.
    return (f'[HB  ] state={state} board={board} btn=up ble=absent/armed | sense: {mv}mV '
            f'({raw}) | heap=100000 loopMax=900us | wifi=up rssi=-60 mqtt=up')


OFF_BY_SENSE = '[ACT ] TPMS1 LOW while ON, board shut down -> powering off'
CASES = {
    'healthy shutdown': ([], [(0, hb('ON', 'UP', 2900, 'high')), (1, hb('ON', 'UP', 1500, 'high')),
                              (2, hb('ON', 'UP', 120, 'low')), (3, OFF_BY_SENSE),
                              (3, hb('OFF', 'DOWN', 40, 'low')), (4, hb('OFF', 'DOWN', 30, 'low'))]),
    'dead band': (['SENSE_DEADBAND'], [(t, hb('ON', 'UP', 1100, 'high')) for t in range(8)]),
    'chatter': (['SENSE_CHATTER'],
                [(t, hb('ON', 'UP', 700 if t % 2 else 900, 'low' if t % 2 else 'high'))
                 for t in range(8)]),
    'psu still on': (['PSU_STILL_ON'],
                     [(0, '[ACT ] long press (>5s) while ON -> powering off')] +
                     [(t, hb('OFF', 'UP', 2900, 'high')) for t in range(1, 10)]),
    'residual': (['SENSE_RESIDUAL'], [(t, hb('OFF', 'UP', 1200, 'high')) for t in range(8)]),
    'ble rewake': (['REWAKE'], [(0, OFF_BY_SENSE),
                                (20, '[ACT ] controller present (BLE) -> powering on')]),
    'button is not rewake': ([], [(0, OFF_BY_SENSE),
                                  (20, '[ACT ] short press while OFF -> powering on')]),
    'portal psu': (['PORTAL_PSU_ON'], [(0, '[PORTAL] host power ON')]),
    'reset while on': (['RESET'], [(0, hb('ON', 'UP', 2900, 'high')),
                                   (1, '[INIT] reset reason: BROWNOUT (15)')]),
}


def power_off(after_mv, raw):
    # 10 s on at 2900 mV, power-off at t=10, then 40 s of the given level.
    return ([(t, hb('ON', 'UP', 2900, 'high')) for t in range(10)] + [(10, OFF_BY_SENSE)] +
            [(t, hb('OFF', 'DOWN', after_mv, raw)) for t in range(10, 50)])


# name: (warn codes, OFF_TRACE verdicts, lines)
TRACES = {
    'trace drop': ([], ['drop'], power_off(60, 'low')),
    'trace mid': (['SENSE_RESIDUAL', 'OFF_TRACE'], ['mid'], power_off(1200, 'high')),
    'trace high': (['PSU_STILL_ON', 'OFF_TRACE'], ['high'], power_off(2900, 'high')),
}

# Saved-file formats: the page's own header + stamps, and a bare firmware log.
deadband = [hb('ON', 'UP', 1100, 'high') for _ in range(8)]
TEXTS = {
    'own format': '# BC250 power log\n# ---\n' + '\n'.join(
        f'[2026-10-18 12:00:{i:02d}.000 +{i}.000] {line}' for i, line in enumerate(deadband)),
    'bare log': '\n'.join(deadband),
}

harness = analyzer + r'''
const input = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const out = { cases: {}, texts: {}, traces: {} };
for (const [name, [, lines]] of Object.entries(input.cases)) {
  const an = new Analyzer();
  for (const [t, line] of lines) an.feed(t, line);
  out.cases[name] = an.findings.filter((f) => f.severity === 'warn').map((f) => f.code);
}
for (const [name, [, , lines]] of Object.entries(input.traces)) {
  const an = new Analyzer();
  for (const [t, line] of lines) an.feed(t, line);
  an.finish();
  const tr = an.findings.filter((f) => f.code === 'OFF_TRACE');
  out.traces[name] = {
    warn: an.findings.filter((f) => f.severity === 'warn').map((f) => f.code),
    verdicts: tr.map((f) => f.params.verdict),
    before: tr.map((f) => f.params.before),
  };
}
for (const [name, text] of Object.entries(input.texts))
  out.texts[name] = analyseText(text).findings.map((f) => f.code);
console.log(JSON.stringify(out));
'''

with tempfile.TemporaryDirectory() as d:
    js = Path(d) / 'harness.js'
    js.write_text(harness, encoding='utf-8')
    res = subprocess.run(['node', str(js)], input=json.dumps({'cases': CASES, 'texts': TEXTS, 'traces': TRACES}),
                         capture_output=True, text=True, check=True)
out = json.loads(res.stdout)

for name, (want, _) in CASES.items():
    assert out['cases'][name] == want, f'{name}: {out["cases"][name]} != {want}'
for name, (warn, verdicts, _) in TRACES.items():
    got = out['traces'][name]
    assert got['warn'] == warn, f'{name}: {got["warn"]} != {warn}'
    assert got['verdicts'] == verdicts, f'{name}: {got["verdicts"]} != {verdicts}'
    assert got['before'] == [2900], f'{name}: before {got["before"]}'
for name, got in out['texts'].items():
    assert got == ['SENSE_DEADBAND'], f'{name}: {got}'

print('power-log analyzer host tests passed')
