// NODE_PATH may point to the local Playwright installation. No daemon required.
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const traySource = fs.readFileSync(path.join(root, 'src-tauri/src/tray.rs'), 'utf8');
const width = Number(traySource.match(/const PANEL_WIDTH: f64 = ([\d.]+);/)[1]);
const height = Number(traySource.match(/const PANEL_HEIGHT: f64 = ([\d.]+);/)[1]);
const catalog = JSON.parse(fs.readFileSync(path.join(root, 'shell/tray-locales.json'), 'utf8'));
const labels = catalog.en;
const languages = fs.readFileSync(path.join(root, '../client/internal/status/ui/src/i18n/languages.ts'), 'utf8');
const codes = [...languages.matchAll(/code: "([^"]+)"/g)].map(m => m[1]);
assert.deepEqual(Object.keys(catalog).sort(), codes.sort());
for (const code of codes) {
  assert.deepEqual(Object.keys(catalog[code]).sort(), Object.keys(labels).sort(), code);
  assert.ok(Object.values(catalog[code]).every(value => typeof value === 'string' && value.trim()), code);
}

(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  try {
    const page = await browser.newPage({ viewport: { width, height } });
    await page.exposeFunction('resizeTray', async requested => {
      await page.setViewportSize({ width, height: Math.max(height, Math.min(900, Math.ceil(requested))) });
    });
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    await page.addInitScript(({ labels }) => {
      window.snapshot = { name: 'eq-cbf1ae3c', org: 'CalabiTeam', ip: '100.64.0.36',
        status: 'Connected', up: true, online: 3, files: true, locale: 'en', labels };
      window.actions = [];
      window.__TAURI_INTERNALS__ = { invoke: async (_, { action, height }) => {
        if (action === 'snapshot') return window.snapshot;
        if (action === 'resize') { await window.resizeTray(height); return; }
        window.actions.push(action);
      }};
    }, { labels });
    await page.goto('file:///' + path.join(root, 'shell/tray.html').replaceAll('\\', '/'));
    await page.getByText('Open main window', { exact: true }).waitFor();
    for (const colorScheme of ['light', 'dark']) {
      await page.emulateMedia({ colorScheme });
      const surface = await page.evaluate(() => ({
        background: getComputedStyle(document.querySelector('.menu')).backgroundColor,
        body: getComputedStyle(document.body).backgroundColor,
        root: getComputedStyle(document.documentElement).backgroundColor,
      }));
      assert.equal(surface.background, 'rgb(255, 255, 255)', colorScheme);
      assert.equal(surface.body, 'rgba(0, 0, 0, 0)', colorScheme);
      assert.equal(surface.root, 'rgba(0, 0, 0, 0)', colorScheme);
    }
    assert.equal(await page.locator('#peers').innerText(), '3 online');
    await page.getByRole('button', { name: 'Restart service', exact: true }).click();
    assert.deepEqual(await page.evaluate(() => window.actions), ['restart']);
    assert.ok(await page.locator('#mesh-toggle').isDisabled());
    for (const [action, key] of [['mesh-down','停止组网'], ['mesh-up','启动组网']]) {
      await page.evaluate(action => { snapshot.mesh_action=action; return update(); }, action);
      await page.getByRole('button', {name: labels[key], exact:true}).click();
      assert.equal(await page.evaluate(() => window.actions.at(-1)), action);
    }
    await page.locator('#copy').click();
    assert.equal(await page.locator('#receipt').innerText(), 'Mesh IP copied');
    await page.evaluate(() => { snapshot.mesh_action = 'mesh-down'; return update(); });
    await page.waitForFunction(() => !document.getElementById('receipt').textContent);
    for (const locale of [...codes, 'en']) {
      await page.evaluate(({ locale, catalog }) => {
        snapshot.locale = locale;
        snapshot.labels = catalog[locale];
        snapshot.status = catalog[locale]['组网已连接'];
        return update();
      }, { locale, catalog });
      assert.equal(await page.locator('html').getAttribute('lang'), locale);
      assert.equal(await page.locator('[data-action="open"] > span').first().innerText(), catalog[locale]['打开主窗口']);
      const overflow = await page.evaluate(() => [...document.querySelectorAll('.row, .header, .statusline, .copy')]
        .filter(el => el.scrollWidth > el.clientWidth || el.getBoundingClientRect().right > innerWidth).map(el => el.className));
      assert.deepEqual(overflow, [], locale);
      assert.ok(await page.evaluate(() => document.documentElement.scrollHeight <= innerHeight), locale);
      for (const key of ['正在检查连接…', '后台服务不可用', '组网状态暂不可用', '组网已暂停', '组网未开启', '组网连接中']) {
        await page.evaluate(key => {
          snapshot.status = snapshot.labels[key]; snapshot.ip = ''; snapshot.online = null;
          return update();
        }, key);
        assert.ok(await page.evaluate(() => document.documentElement.scrollHeight <= innerHeight), locale + ': ' + key);
        assert.equal(await page.locator('#peers').innerText(), catalog[locale]['状态待确认']);
      }
      await page.evaluate(() => { snapshot.status = snapshot.labels['组网已连接']; snapshot.ip = '100.64.0.36'; snapshot.online = 3; return update(); });
      console.log('Layout OK:', locale);
      if (process.env.TRAY_SCREENSHOT_DIR) await page.screenshot({ path: path.join(process.env.TRAY_SCREENSHOT_DIR, locale + '.png') });
    }
    if (process.env.TRAY_SCREENSHOT) await page.screenshot({ path: process.env.TRAY_SCREENSHOT });
    await page.evaluate(() => {
      snapshot.name = 'A-long-device-name-'.repeat(10);
      snapshot.org = 'A long organization name '.repeat(10);
      return update();
    });
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    assert.ok(await page.evaluate(() => document.documentElement.scrollHeight <= innerHeight));
    const emulation = await page.context().newCDPSession(page);
    for (const deviceScaleFactor of [1, 1.5, 2]) {
      await emulation.send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor, mobile: false });
      assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth && document.documentElement.scrollHeight <= innerHeight), 'DPR ' + deviceScaleFactor);
      const quit = await page.locator('[data-action="quit"]').boundingBox();
      assert.ok(quit.y + quit.height <= height - 6, 'last action clipped at DPR ' + deviceScaleFactor);
    }
    await emulation.detach();
    // DPR changes do not exercise Windows Accessibility > Text size. Grow all
    // text like that setting while preserving the selected font and layout.
    const largerText = await page.addStyleTag({ content: `
      body{font-size:18px}.name{font-size:19.5px}
      .local,.org,.statusline,.copy,.meta,.receipt{font-size:16.5px}
    ` });
    for (const locale of codes) {
      await page.evaluate(({ locale, catalog }) => {
        snapshot.locale=locale; snapshot.labels=catalog[locale]; snapshot.status=catalog[locale]['组网已连接']; return update();
      }, { locale, catalog });
      await page.waitForFunction(() => innerHeight > 336 && document.querySelector('.menu').getBoundingClientRect().bottom <= innerHeight - 6);
      const geometry = await page.evaluate(() => ({
        scroll: document.documentElement.scrollHeight > innerHeight,
        quit: document.querySelector('[data-action="quit"]').getBoundingClientRect().bottom,
        height: innerHeight,
      }));
      assert.equal(geometry.scroll, false, locale + ': enlarged text');
      assert.ok(geometry.quit <= geometry.height - 6, locale + ': enlarged last action');
      await page.locator('[data-action="quit"]').focus();
      assert.equal(await page.evaluate(() => scrollY), 0, locale + ': focus must not scroll device header away');
      if (locale === 'zh-CN' && process.env.TRAY_LARGE_TEXT_SCREENSHOT) await page.screenshot({ path: process.env.TRAY_LARGE_TEXT_SCREENSHOT });
    }
    await largerText.evaluate(node => node.remove());
    await page.evaluate(labels => { snapshot.locale='en'; snapshot.labels=labels; return update(); }, labels);
    await page.waitForFunction(() => innerHeight === 336);
    // Model WebView text zoom separately from monitor DPI. CSS viewport pixels
    // are smaller in count than native logical pixels; ui.2 treated them alike.
    for (const zoom of [1.15, 1.25, 1.5, 2]) {
      const scaled = await browser.newPage({ viewport: { width: Math.floor(width / zoom), height: Math.floor(height / zoom) } });
      let nativeHeight = height;
      await scaled.exposeFunction('resizeScaledTray', async (requested, viewport) => {
        // Missing viewport reproduces the old CSS-as-logical resize bug.
        nativeHeight = Math.max(height, Math.ceil(viewport ? requested * nativeHeight / viewport : requested));
        await scaled.setViewportSize({ width: Math.floor(width / zoom), height: Math.floor(nativeHeight / zoom) });
      });
      await scaled.addInitScript(({ labels }) => {
        window.__TAURI_INTERNALS__ = { invoke: async (_, args) => {
          if (args.action === 'snapshot') return { name:'eq-d12444f3', org:'CalabiTeam', status:'Connected', ip:'100.64.0.19', up:true, online:4, files:true, mesh_action:'mesh-down', labels, locale:'zh-CN' };
          if (args.action === 'resize') return window.resizeScaledTray(args.height, args.viewportHeight);
        }};
      }, { labels: catalog['zh-CN'] });
      await scaled.goto('file:///' + path.join(root, 'shell/tray.html').replaceAll('\\', '/'));
      await scaled.waitForFunction(() => document.querySelector('.menu').getBoundingClientRect().bottom <= innerHeight - 6);
      assert.ok(await scaled.evaluate(() => document.documentElement.scrollHeight <= innerHeight), 'WebView zoom ' + zoom);
      await scaled.locator('[data-action="quit"]').focus();
      assert.equal(await scaled.evaluate(() => scrollY), 0, 'WebView zoom focus ' + zoom);
      await scaled.close();
    }
    await page.locator('[data-action="open"]').focus();
    await page.keyboard.press('ArrowDown');
    assert.ok(await page.locator('[data-action="mesh"]').evaluate(el => el === document.activeElement));
    await page.keyboard.press('Escape');
    assert.equal(await page.evaluate(() => window.actions.at(-1)), 'hide');
    for (const status of ['Service unavailable', 'Mesh status unavailable', 'Connecting…']) {
      await page.evaluate(status => { snapshot.status = status; snapshot.ip = ''; snapshot.online = null; snapshot.files = false; return update(); }, status);
      assert.ok(await page.locator('#copy').isDisabled());
      assert.ok(await page.locator('#files').isDisabled());
      assert.equal(await page.locator('#peers').innerText(), 'Unknown');
    }
    // Exercise the real injected bridge against a fake daemon origin, including
    // startup rejection, live switching, and non-daemon navigation.
    const bridge = await browser.newPage();
    await bridge.route('http://127.0.0.1:7400/**', r => r.fulfill({ body: '<html lang="en"><body></body></html>', contentType: 'text/html' }));
    await bridge.addInitScript(() => {
      window.calls = [];
      let first = true;
      window.__TAURI_INTERNALS__ = { invoke: async (cmd, args) => {
        if (first) { first = false; throw Error('Tray not ready'); }
        window.calls.push({ cmd, ...args });
      }};
    });
    await bridge.addInitScript({ path: path.join(root, 'shell/tray-language.js') });
    await bridge.goto('http://127.0.0.1:7400/');
    await bridge.waitForFunction(() => calls.some(c => c.locale === 'en'));
    for (const code of codes) {
      await bridge.evaluate(code => { document.documentElement.lang = code; }, code);
      await bridge.waitForFunction(code => calls.some(c => c.locale === code), code);
    }
    assert.ok((await bridge.evaluate(() => calls)).every(c => c.cmd === 'tray_set_locale'));
    await bridge.goto('about:blank');
    await bridge.waitForTimeout(1100);
    assert.deepEqual(await bridge.evaluate(() => calls), []);
    assert.deepEqual(errors, []);
    console.log('PASS: 10 languages, light/transparent surface, long names, DPR 1/1.5/2, enlarged system text with native height fitting, keyboard, copy, states and language bridge.');
  } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
