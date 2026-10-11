// Temporary localhost portal with fictional in-memory device responses.
// Run with Node.js and Playwright (Chromium or Microsoft Edge installed).
const http = require('node:http');
const fs = require('node:fs/promises');
const path = require('node:path');
const { chromium } = require('playwright');
const root = path.resolve(__dirname, '..');

(async () => {
  const html = await fs.readFile(path.join(root, 'app/index.html'));
  const out = path.join(root, 'docs/images/portal');
  await fs.mkdir(out, { recursive: true });
  const settings = { wifiSsid: 'Home_2.4G', wifiPassSet: false, wakeOnLan: true,
    wolMac: '02:00:00:00:00:01', mqttHost: '192.168.0.10', mqttPort: 1883,
    mqttUser: 'bc250', mqttPassSet: false, deviceName: 'BC250', wakeAddrs: [], btHost: '' };
  let psSession = false;
  const server = http.createServer(async (req, res) => {
    try {
      const url = new URL(req.url, 'http://localhost');
      if (!url.pathname.startsWith('/api/')) {
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
        res.end(html); return;
      }
      let body = '';
      for await (const chunk of req) body += chunk;
      const data = body ? JSON.parse(body) : {};
      let reply = { ok: true };
      switch (url.pathname) {
        case '/api/status': reply = { classicSupported: true, psSession, wakeAddrs: [] }; break;
        case '/api/settings': reply = settings; break;
        case '/api/wifi/scan': reply = { networks: [{ ssid: 'Home_2.4G', rssi: -48, open: false }] }; break;
        case '/api/host': reply = { hostAddr: settings.btHost, learned: settings.btHost ? null : '02:00:00:00:10:01', boardUp: false, psuOn: false, learning: true }; break;
        case '/api/host/select': settings.btHost = data.addr; break;
        case '/api/host/controller-step': psSession = true; break;
        case '/api/ble/devices': reply = { classicSupported: true, psSession: true, scanning: true,
          bleScanning: true, classicScanning: true, capacity: 48, lastSeenAgeMs: 500,
          devices: [{ name: 'PlayStation (PS button)', addr: '02:00:00:00:20:01', rssi: -127, transport: 'classic' },
            { name: 'Xbox Wireless Controller', addr: '02:00:00:00:20:02', rssi: -55, transport: 'ble' }] }; break;
        case '/api/wifi': settings.wifiSsid = data.ssid; settings.wakeOnLan = data.wakeOnLan; break;
        case '/api/ble/select': settings.wakeAddrs = data.addrs; break;
      }
      res.writeHead(200, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(reply));
    } catch (error) { res.writeHead(500); res.end(JSON.stringify({ error: error.message })); }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  try {
    browser = await chromium.launch({ channel: 'msedge', headless: true });
    const page = await browser.newPage({ viewport: { width: 480, height: 1000 }, deviceScaleFactor: 2, locale: 'ko-KR' });
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    const url = `http://127.0.0.1:${server.address().port}`;
    console.log(`Temporary demo: ${url}`);
    await page.goto(url);
    await page.locator('#screen-start').waitFor({ state: 'visible' });
    const shot = async name => {
      await page.evaluate(() => document.fonts.ready);
      await page.locator('.wrap').screenshot({ path: path.join(out, name + '.png') });
    };
    await shot('start');
    await page.locator('#start-btn').click();
    await page.locator('#wifi-list .dev').waitFor();
    await page.locator('#wifi-pw').fill('ExampleOnly123');
    await shot('wifi');
    await page.locator('#wifi-btn').click();
    await page.locator('#mq-pw').fill('ExampleOnly123');
    await shot('mqtt');
    await page.locator('#mqtt-btn').click();
    await page.locator('#ps-found').waitFor({ state: 'visible' });
    await shot('host');
    await page.locator('#ps-save-host').click();
    await page.locator('#ps-controller-step').click();
    await page.locator('#ps-msg').filter({ hasText: '재부팅 중' }).waitFor();
    await page.reload();
    await page.locator('#dev-list .dev').first().waitFor();
    await page.locator('#dev-list .dev').first().click();
    await shot('controllers');
    await page.locator('#save-btn').click();
    await page.locator('#screen-done').waitFor({ state: 'visible' });
    await shot('done');
    if (errors.length) throw new Error(errors.join('\n'));
    console.log('Captured six example screens; no browser script errors.');
  } finally {
    if (browser) await browser.close();
    await new Promise(resolve => server.close(resolve));
    console.log('Temporary server stopped.');
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
