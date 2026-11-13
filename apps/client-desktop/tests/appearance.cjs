// Built SPA against an explicitly supplied fake daemon; never defaults to :7400.
// CALABI_UI_TEST_URL=http://127.0.0.1:<mock-port> node tests/appearance.cjs
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const base = process.env.CALABI_UI_TEST_URL;
if (!base) throw Error('Set CALABI_UI_TEST_URL to an isolated fake daemon serving the built SPA.');
const source = path.resolve(__dirname, '../../client/internal/status/ui/src');
const labels = code => JSON.parse(fs.readFileSync(path.join(source, 'i18n/locales', code + '.json'), 'utf8')).appearance;

(async () => {
  const browser = await chromium.launch({ channel:'msedge', headless:true });
  try {
    const context = await browser.newContext({ viewport:{width:1100,height:720}, colorScheme:'light' });
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    await page.addInitScript(() => localStorage.setItem('calabi_lang', 'en'));
    await page.goto(base + '/#/settings');
    const scheme = value => page.waitForFunction(value => document.documentElement.dataset.theme === value, value);
    const pick = async (value, code='en') => {
      await page.locator('.client-account-menu').click();
      await page.waitForTimeout(300); // Finish the account popup's enter motion before opening its submenu.
      await page.locator('.ant-dropdown-menu-submenu-title').filter({hasText:labels(code).title}).hover();
      await page.locator('.appearance-menu-pick:visible').first().waitFor();
      await page.waitForTimeout(500); // Let the submenu's enter animation settle before moving the pointer.
      await page.getByRole('menuitem', {name:labels(code)[value], exact:true}).click();
      await page.waitForTimeout(300); // Finish closing before a subsequent selection reopens the popup.
    };
    await page.locator('.client-account-menu').waitFor();
    await scheme('light');
    assert.equal(await page.evaluate(() => document.documentElement.dataset.appearanceMode), 'system');
    await page.emulateMedia({ colorScheme:'dark' }); await scheme('dark');
    await pick('light'); await scheme('light');
    await page.reload(); await page.locator('.client-account-menu').waitFor(); await scheme('light');
    assert.equal(await page.evaluate(() => localStorage.getItem('calabi.client.theme')), 'light');
    await pick('dark'); await scheme('dark');
    await page.emulateMedia({colorScheme:'light'}); await scheme('dark');
    await pick('system'); await scheme('light');
    await page.emulateMedia({colorScheme:'dark'}); await scheme('dark');

    // Same local origin across desktop webviews: another window's preference
    // must propagate, and a cleared preference must go back to the OS.
    const second = await context.newPage(); await second.goto(base + '/#/settings');
    await second.evaluate(() => localStorage.setItem('calabi.client.theme','light')); await scheme('light');
    await second.evaluate(() => localStorage.removeItem('calabi.client.theme')); await scheme('dark');
    await second.close();

    for (const mode of ['light','dark']) {
      await pick(mode); await scheme(mode);
      for (const route of ['overview','mesh','mesh/files','mesh/services','endpoints','logs','tools','settings']) {
        await page.goto(base + '/#/' + route);
        await page.locator('.client-sheet').waitFor();
        await page.waitForTimeout(250);
        const actual = await page.evaluate(() => ({
          canvas:getComputedStyle(document.body).backgroundColor,
          sheet:getComputedStyle(document.querySelector('.client-sheet')).backgroundColor,
          overflow:document.documentElement.scrollWidth > innerWidth,
        }));
        assert.equal(actual.canvas,mode==='light'?'rgb(236, 238, 244)':'rgb(11, 15, 26)',route);
        assert.equal(actual.sheet,mode==='light'?'rgb(255, 255, 255)':'rgb(17, 22, 36)',route);
        assert.equal(actual.overflow,false,route);
      }
      if(process.env.APPEARANCE_SCREENSHOT_DIR) await page.screenshot({path:path.join(process.env.APPEARANCE_SCREENSHOT_DIR,`client-${mode}.png`)});
    }
    for (const code of ['de','en','es','fr','it','pt','ja','ko','zh-CN','zh-TW']) {
      // A fresh page avoids the English-only init hook on the main test page.
      const localized = await context.newPage();
      await localized.addInitScript(code => {localStorage.setItem('calabi_lang',code);localStorage.setItem('calabi.client.theme','system');},code);
      await localized.goto(base+'/#/settings');
      await localized.locator('.client-account-menu').click();
      await localized.locator('.ant-dropdown-menu-submenu-title').filter({hasText:labels(code).title}).hover();
      await localized.locator('.appearance-menu-pick:visible').first().waitFor();
      const options=localized.locator('.appearance-menu-pick:visible');
      assert.equal(await options.count(),3,code);
      for(const option of await options.all())assert.ok(await option.evaluate(el=>el.scrollWidth<=el.clientWidth),code);
      assert.equal(await localized.locator('.appearance-select').count(),0,code);
      await localized.close();
    }
    await page.request.post(base + '/v1/__set', { data:{standalone:true} });
    await page.evaluate(()=>localStorage.setItem('calabi_lang','en'));
    await page.reload();
    await page.locator('.client-account-menu').waitFor();
    for(const mode of ['light','dark']){
      await page.goto(base+'/#/settings');await pick(mode);await scheme(mode);
      await page.goto(base+'/#/overview');await page.locator('.client-sheet').waitFor();
      assert.equal(await page.evaluate(()=>document.documentElement.dataset.theme),mode);
    }
    await page.request.post(base + '/v1/__set', { data:{standalone:false} });
    // Native titlebar bridge: only the local daemon page can ask for it, and
    // system mode releases the native override rather than forcing dark/light.
    const bridge = await context.newPage();
    await bridge.addInitScript(() => {window.themeCalls=[];window.__TAURI_INTERNALS__={invoke:async(cmd,args)=>themeCalls.push({cmd,...args})};});
    await bridge.addInitScript({path:path.resolve(__dirname,'../shell/appearance.js')});
    await bridge.goto(base+'/#/settings');
    await bridge.waitForFunction(()=>themeCalls.some(c=>c.cmd==='desktop_set_theme'));
    await bridge.evaluate(()=>{document.documentElement.dataset.appearanceMode='system';});
    await bridge.waitForFunction(()=>themeCalls.some(c=>c.mode==='system'));
    await bridge.goto('about:blank');
    await bridge.waitForTimeout(1100);
    assert.deepEqual(await bridge.evaluate(()=>themeCalls),[]);
    const remote = await context.newPage();
    remote.on('pageerror',e=>errors.push(e.message));
    await remote.addInitScript(()=>{
      localStorage.setItem('calabi_lang','en');
      window.remoteOpenCount=0;
      window.__TAURI_INTERNALS__={invoke:async(command,args)=>{
        if(command==='product_info')return{product:'desktop',version:'fixture',bundled_core_version:'fixture',bundled_core_commit:'fixture'};
        if(command!=='files_action')return{};
        const action=args.action.action;
        if(action==='supported')return{supported:true};
        if(action==='rdp_history'||action==='ssh_history')return[];
        if(action==='rdp_open'){window.remoteOpenCount++;return new Promise((resolve,reject)=>{window.remoteOpenReply={resolve,reject};});}
        return{available:true,rdp_supported:true,rdp_targets:['k1'],peers:[],jobs:[],sources:[],received:[],receiving:false};
      }};
    });
    await remote.goto(base+'/#/mesh');
    const menu=()=>remote.locator('.ant-table-row[data-row-key="k1"] button[aria-label]').last();
    const open=async()=>{await menu().click();await remote.getByRole('menuitem',{name:'Remote login',exact:true}).click();};
    const remoteLabels=JSON.parse(fs.readFileSync(path.join(source,'i18n/locales/en.json'),'utf8')).remote;
    for(const [nativeError,label] of [
      ['Local file agent is busy; try again shortly','agentBusy'],
      ['Local file agent response timed out; retry the operation','agentTimeout'],
      ['Remote window is not responding; try again shortly','windowBusy'],
      ['Timed out connecting to the Windows file agent','agentTimeout'],
    ]){
      const count=await remote.evaluate(()=>remoteOpenCount);
      await open();await remote.getByText(remoteLabels.opening,{exact:true}).waitFor();
      await open();assert.equal(await remote.evaluate(()=>remoteOpenCount),count+1,'duplicate pending open');
      await remote.evaluate(error=>remoteOpenReply.reject(error),nativeError);
      await remote.getByText(remoteLabels[label],{exact:true}).last().waitFor();
      await remote.waitForFunction(()=>!document.querySelector('.ant-message-loading'));
    }
    await open();await remote.getByText(remoteLabels.opening,{exact:true}).waitFor();
    await remote.evaluate(()=>remoteOpenReply.resolve({}));await remote.waitForFunction(()=>!document.querySelector('.ant-message-loading'));
    assert.deepEqual(errors,[]);
    console.log('PASS: console themes, OS/storage, 8 routes, 10 locales, native bridge, RDP pending feedback, duplicate suppression, localized errors and retry.');
  } finally { await browser.close(); }
})().catch(error=>{console.error(error);process.exitCode=1;});
