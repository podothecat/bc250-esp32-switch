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


def chb(state, ble, arm):
    # Heartbeat with the controller fields set; sense is a quiet OFF level.
    up = state != 'OFF'
    return (f'[HB  ] state={state} board={"UP" if up else "DOWN"} btn=up ble={ble}/{arm} | sense: '
            f'{2900 if up else 40}mV ({"high" if up else "low"}) | heap=100000 loopMax=900us | '
            f'wifi=up rssi=-60 mqtt=up | fw=test')


def btdiag(ready, adv):
    return (f'[BTDIAG] ready={ready} enabled=1 ble=1 allowlist=0 adv={adv} le_conn=0 status=0x00 '
            f'disconnects=0 reason=0x00 cmd_errors=0 opcode=0x0000 cmd_status=0x00')


MARK = '[diag] mark: controller on'
CTRL_ADV = '[BLE ] registered controller advertising: aa:bb:cc:dd:ee:ff'
CTRL_WAKE = '[ACT ] controller present (BLE) -> powering on'
CASES.update({
    'no controller bound': (['CTRL_NONE_BOUND'], [(0, '[BT] no controller bound; wake disabled')]),
    'scan failed': (['CTRL_SCAN_FAILED'],
                    [(0, '[BT] scan requested for 1 controller(s): failed; will retry')]),
    'scan started': ([], [(0, '[BT] scan requested for 2 controller(s): started')]),
    'bt not ready': (['CTRL_BT_NOT_READY'], [(t * 5, btdiag(0, 0)) for t in range(4)]),
    'no advertisements': (['CTRL_NO_ADV'], [(t * 5, btdiag(1, 120)) for t in range(8)]),
    'advertisements flowing': ([], [(t * 5, btdiag(1, 120 + t * 40)) for t in range(8)]),
    'ps off, board on at boot': (['CTRL_PS_BOARD_ON'], [
        (0, '[BTHOST] addr=aa:bb:cc:dd:ee:ff spoofed=0 reason=board is on connectable=0 discoverable=0')]),
    'ps spoofed': ([], [
        (0, '[BTHOST] addr=aa:bb:cc:dd:ee:ff spoofed=1 reason=board off with saved host connectable=1 discoverable=0')]),
    'never quiet': (['CTRL_NEVER_QUIET'],
                    [(0, '[BLE ] wake disarmed until controller goes quiet')] +
                    [(t, chb('OFF', 'present', 'disarmed')) for t in range(1, 130)]),
    'quiet re-arms': ([], [(0, '[BLE ] wake disarmed until controller goes quiet')] +
                      [(t, chb('OFF', 'present' if t < 10 else 'absent', 'disarmed')) for t in range(1, 40)] +
                      [(40, '[BLE ] controller quiet -> wake re-armed')] +
                      [(t, chb('OFF', 'absent', 'armed')) for t in range(41, 140)]),
    'mark not seen': (['CTRL_MARK_NOT_SEEN'], [(t, chb('OFF', 'absent', 'armed')) for t in range(3)] +
                      [(3, MARK)] + [(t, chb('OFF', 'absent', 'armed')) for t in range(4, 20)]),
    'mark woke': ([], [(0, chb('OFF', 'absent', 'armed')), (1, MARK), (3, CTRL_ADV), (3, CTRL_WAKE)] +
                  [(t, chb('BOOTING', 'present', 'armed')) for t in range(4, 20)]),
    'mark seen, paused': (['CTRL_MARK_NO_WAKE'],
                          [(0, '[BLE ] wake disarmed until controller goes quiet'),
                           (1, chb('OFF', 'absent', 'disarmed')), (2, MARK), (4, CTRL_ADV)] +
                          [(t, chb('OFF', 'present', 'disarmed')) for t in range(5, 20)]),
    'mark while on': ([], [(0, chb('ON', 'absent', 'armed')), (1, MARK)] +
                      [(t, chb('ON', 'present', 'armed')) for t in range(2, 20)]),
})

# Setup-portal registration. Each boot starts with the reset line, then setup mode.
SETUP = ['[INIT] reset reason: SW (3)', '=== BC250 PSU controller : SETUP MODE ===']
CLIENT = '[PORTAL] AP client connected; clients=1 heap=100000'
DROP = '[PORTAL] AP client disconnected; clients=0 heap=100000'


def boot(t, identity):
    return [(t, SETUP[0]), (t, SETUP[1]), (t, f'[PORTAL] host identity: {identity}, board down')]


HOST_SESSION = boot(0, 'BC250 Power Setup (no saved host)')
PS_SESSION = boot(100, 'spoofing BC250 (PS learning with saved host)')
PAIR_FLOW = (HOST_SESSION + [(5, CLIENT), (10, '[PORTAL] host power ON'),
                             (30, '[PORTAL] BC250 candidate seen: 11:22:33:44:55:66'),
                             (35, '[PORTAL] BC250 host 11:22:33:44:55:66'),
                             (40, '[PORTAL] host power OFF'),
                             (50, '[PORTAL] controller step requested; rebooting into spoofed session')] +
             PS_SESSION + [(105, CLIENT), (110, '[PORTAL] BLE scan started'),
                           (115, '[PORTAL] device aa:bb:cc:dd:ee:ff ble rssi=-60 name=Xbox Wireless Controller'),
                           (120, '[PORTAL] bound 1 controller(s)'),
                           (125, '[PORTAL] setup complete, restarting into normal mode')])
CASES.update({
    'pair flow': (['PORTAL_PSU_ON'], PAIR_FLOW),
    'pair no phone': (['PAIR_NO_CLIENT'], boot(0, 'BC250 Power Setup (no saved host)') + [(100, btdiag(1, 0))]),
    'pair phone drops': (['PAIR_CLIENT_DROPS'], HOST_SESSION + [(t, CLIENT if t % 2 else DROP) for t in range(1, 8)]),
    'pair host not seen': (['PORTAL_PSU_ON', 'PAIR_HOST_NOT_SEEN'], HOST_SESSION + [(5, CLIENT), (10, '[PORTAL] host power ON'),
                                                                     (200, btdiag(1, 0))]),
    'pair step not spoofed': (['PAIR_STEP_NOT_SPOOFED'],
                              HOST_SESSION + [(5, CLIENT),
                                              (50, '[PORTAL] controller step requested; rebooting into spoofed session')] +
                              boot(60, 'BC250 Power Setup (board is on)')),
    'pair picker without host': (['PAIR_PICKER_NO_HOST'], HOST_SESSION + [(5, CLIENT), (10, '[PORTAL] BLE scan started')]),
    'pair nothing found': (['PAIR_NOTHING_FOUND'], PS_SESSION + [(105, CLIENT), (110, '[PORTAL] BLE scan started')] +
                           [(110 + t * 5, btdiag(1, 500 + t * 40)) for t in range(14)]),
    'pair not saved': (['PAIR_NOT_SAVED'], PS_SESSION + [
        (105, CLIENT), (110, '[PORTAL] BLE scan started'),
        (115, '[PORTAL] device 00:11:22:33:44:55 classic rssi=-127 name=PlayStation (PS button)'),
        (130, '[PORTAL] setup complete, restarting into normal mode')]),
    'pair cleared': (['PAIR_BOUND_ZERO'], PS_SESSION + [(105, CLIENT), (120, '[PORTAL] bound 0 controller(s)')]),
    'pair hid timeout': (['PAIR_HID_TIMEOUT'], HOST_SESSION + [(5, CLIENT),
                                                              (20, '[HID] slot=0 first input report timed out; disconnecting')]),
})

# The checklist for PAIR_FLOW: every step reached, each at its line's time.
PAIR_STEPS_WANT = {'setup': 0, 'client': 5, 'candidate': 30, 'host': 35, 'step': 50, 'spoofed': 100,
                   'picker': 110, 'found': 115, 'bound': 120, 'done': 125}


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
{
  const an = new Analyzer();
  for (const [t, line] of input.pairFlow) an.feed(t, line);
  out.pair = { steps: an.pair.steps, devices: an.pair.devices.map((d) => d.addr), bound: an.pair.bound };
}
for (const [name, text] of Object.entries(input.texts))
  out.texts[name] = analyseText(text).findings.map((f) => f.code);
console.log(JSON.stringify(out));
'''

with tempfile.TemporaryDirectory() as d:
    js = Path(d) / 'harness.js'
    js.write_text(harness, encoding='utf-8')
    res = subprocess.run(['node', str(js)], input=json.dumps({'cases': CASES, 'texts': TEXTS, 'traces': TRACES,
                                     'pairFlow': PAIR_FLOW}),
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

assert out['pair']['steps'] == PAIR_STEPS_WANT, out['pair']['steps']
assert out['pair']['devices'] == ['aa:bb:cc:dd:ee:ff'], out['pair']['devices']
assert out['pair']['bound'] == 1, out['pair']['bound']

print('power-log analyzer host tests passed')
