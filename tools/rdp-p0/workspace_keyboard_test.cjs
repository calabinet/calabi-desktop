// Run against the built daemon SPA: node tools/rdp-p0/workspace_keyboard_test.cjs
// Uses isolated sessions and a fake native bridge; never connects to an RDP host.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const http = require('node:http');
const { spawn } = require('node:child_process');
const ui = path.resolve(process.argv[2] || path.join(__dirname, '../../apps/client/internal/status/ui/dist'));
const chromePath = process.env.CHROME || 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const server = http.createServer((req, res) => {
  const name = new URL(req.url, 'http://localhost').pathname;
  if (name.startsWith('/v1/')) { res.setHeader('Content-Type', 'application/json'); res.end('{"locked":false}'); return; }
  const file = path.resolve(ui, '.' + (name === '/' ? '/index.html' : decodeURIComponent(name)));
  if (!file.startsWith(ui + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) { res.writeHead(404); res.end(); return; }
  res.setHeader('Content-Type', { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css' }[path.extname(file)] || 'application/octet-stream');
  res.end(fs.readFileSync(file));
});

(async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'calabi-rdp-keyboard-'));
  const chrome = spawn(chromePath, ['--headless=new', '--disable-gpu', '--no-first-run', '--remote-debugging-port=0', '--user-data-dir=' + profile], { windowsHide: true, stdio: 'ignore' });
  let launchError, socket;
  chrome.on('error', error => { launchError = error; });
  try {
    for (let n = 0; n < 100 && !fs.existsSync(path.join(profile, 'DevToolsActivePort')); n++) {
      if (launchError) throw launchError;
      await sleep(100);
    }
    const port = fs.readFileSync(path.join(profile, 'DevToolsActivePort'), 'utf8').split('\n')[0];
    const tab = await (await fetch(`http://127.0.0.1:${port}/json/new?about:blank`, { method: 'PUT', signal: AbortSignal.timeout(10000) })).json();
    socket = new WebSocket(tab.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => { socket.addEventListener('open', resolve, { once: true }); socket.addEventListener('error', reject, { once: true }); });
    let serial = 0;
    const pending = new Map();
    socket.addEventListener('message', event => {
      const message = JSON.parse(event.data), entry = pending.get(message.id);
      if (!entry) return;
      pending.delete(message.id); clearTimeout(entry.timer);
      message.error ? entry.reject(new Error(JSON.stringify(message.error))) : entry.resolve(message.result);
    });
    const call = (method, params = {}) => new Promise((resolve, reject) => {
      const id = ++serial;
      const timer = setTimeout(() => { pending.delete(id); reject(new Error('CDP timeout: ' + method)); }, 10000);
      pending.set(id, { resolve, reject, timer }); socket.send(JSON.stringify({ id, method, params }));
    });
    const evaluate = async expression => {
      const result = await call('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
      if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
      return result.result.value;
    };
    await call('Page.enable');
    const results = [];
    for (const platform of ['Win32', 'MacIntel']) for (const native of [false, true]) {
      const { identifier } = await call('Page.addScriptToEvaluateOnNewDocument', { source: `
        Object.defineProperty(navigator,'platform',{get:()=>${JSON.stringify(platform)}});
        window.inputs=[]; window.activeSession='first';
        window.__TAURI_INTERNALS__={invoke:async(command,args)=>{
          if(command==='remote_workspace'){
            const r=args.request;
            if(r.op==='rdp_input'){window.inputs.push(r);return;}
            if(r.op==='select')window.activeSession=r.id;
            return {active:window.activeSession,nativeRdp:${native},fullscreen:false,sessions:[{id:'first',name:'First',protocol:'rdp'},{id:'second',name:'Second',protocol:'rdp'}]};
          }
          if(command==='remote_frame'){
            if(args.revision)return [];
            const frame=new Uint8Array(28),header=new DataView(frame.buffer);
            header.setUint32(0,1,true);header.setUint32(4,2,true);header.setUint32(8,2,true);
            return Array.from(frame);
          }
          return {};
        }};` });
      await call('Page.navigate', { url: `http://127.0.0.1:${server.address().port}/?platform=${platform}&native=${native}#/ssh-session` });
      for (let n = 0; n < 100; n++) {
        if (await evaluate("document.activeElement?.matches('canvas[data-remote-keyboard]') && !document.querySelector('.remote-session:not([hidden]) [role=status]')")) break;
        await sleep(100);
      }
      assert.equal(await evaluate("document.activeElement?.matches('canvas[data-remote-keyboard]')"), true, platform + ': focus must be non-editable');
      assert.equal(await evaluate("document.querySelectorAll('.remote-desktop textarea').length"), 0, platform + ': local IME field must not exist');
      await evaluate(`window.inputs=[];
        window.key=(code,down,extra={})=>document.activeElement.dispatchEvent(new KeyboardEvent(down?'keydown':'keyup',{code,key:code,bubbles:true,cancelable:true,...extra}));
        key('KeyN',true,{key:'Process',keyCode:229,isComposing:true});key('KeyN',false);
        key('ShiftLeft',true,{shiftKey:true});key('ShiftLeft',false);
        key('ControlRight',true,{ctrlKey:true});key('Space',true,{ctrlKey:true});key('Space',false,{ctrlKey:true});key('ControlRight',false);
        key('ArrowLeft',true);key('ArrowLeft',false);`);
      await sleep(60);
      const inputs = await evaluate('window.inputs');
      const scans = inputs.map(({ kind, a, b }) => [kind, a, b]);
      assert.deepEqual(scans, [[10,0x31,0],[11,0x31,0],[10,0x2a,0],[11,0x2a,0],[10,0x1d,1],[10,0x39,0],[11,0x39,0],[11,0x1d,1],[10,0x4b,1],[11,0x4b,1]], platform + ': physical codes must reach the remote IME, including Process/229');
      // IMEs can consume a modifier transition while still exposing its state on Slash.
      await evaluate(`window.inputs=[];
        key('Slash',true,{key:'?',shiftKey:true});key('Slash',false,{shiftKey:true});
        key('Slash',true,{key:'/'});key('Slash',false);`);
      await sleep(60);
      assert.deepEqual(await evaluate('window.inputs.map(x=>[x.kind,x.a,x.b])'),
        [[10,0x2a,0],[10,0x35,0],[11,0x35,0],[11,0x2a,0],[10,0x35,0],[11,0x35,0]],
        platform + ': synchronize swallowed Shift transitions before punctuation without sending local text');
      await evaluate(`window.inputs=[];key('ShiftRight',true,{shiftKey:true});
        key('Slash',true,{key:'?',shiftKey:true});key('Slash',false,{shiftKey:true});key('ShiftRight',false);`);
      await sleep(60);
      assert.deepEqual(await evaluate('window.inputs.map(x=>[x.kind,x.a,x.b])'),
        [[10,0x36,0],[10,0x35,0],[11,0x35,0],[11,0x36,0]],
        platform + ': retain the physical right Shift without injecting left Shift');
      await evaluate(`window.inputs=[];key('KeyA',true);window.dispatchEvent(new Event('blur'));`);
      await sleep(60);
      assert.equal((await evaluate('window.inputs')).at(-1).kind, 0, platform + ': window blur must release held keys');
      await evaluate(`document.querySelector('.remote-session:not([hidden]) canvas').blur();`);
      const center = await evaluate(`(()=>{const r=document.querySelector('.remote-session:not([hidden]) canvas').getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()`);
      await call('Input.dispatchMouseEvent', { type: 'mousePressed', button: 'left', clickCount: 1, ...center });
      await call('Input.dispatchMouseEvent', { type: 'mouseReleased', button: 'left', clickCount: 1, ...center });
      assert.equal(await evaluate("document.activeElement?.matches('canvas[data-remote-keyboard]')"), true, platform + ': clicking restores canvas focus');
      await evaluate(`document.querySelector('.remote-edge').click();document.querySelectorAll('.remote-card')[1].click();`);
      await sleep(150);
      assert.equal(await evaluate("document.activeElement?.closest('.remote-session')?.hidden"), false, platform + ': selected session retains focus');
      await evaluate(`window.inputs=[];key('KeyB',true);key('KeyB',false);`);
      await sleep(60);
      assert.deepEqual(await evaluate('window.inputs.map(x=>x.id)'), ['second', 'second'], platform + ': keys must follow session selection');
      if (platform === 'MacIntel') {
        await evaluate(`window.inputs=[];key('KeyC',true,{metaKey:true});`); await sleep(60);
        assert.deepEqual(await evaluate('window.inputs.map(x=>[x.kind,x.a])'), [[10,0x1d],[2,0x2e],[11,0x1d]], 'Mac Command+C remains mapped to remote Ctrl+C');
      }
      assert.equal(await evaluate('window.inputs.some(x=>x.kind===1)'), false, platform + ': no locally composed Unicode text');
      results.push({ platform, native, physicalIme: true, release: true, clickFocus: true, sessionSelection: true });
      await call('Page.removeScriptToEvaluateOnNewDocument', { identifier });
    }
    console.log(JSON.stringify({ passed: results }));
    await call('Browser.close').catch(() => {});
  } finally {
    socket?.close(); chrome.kill(); server.close();
    for (let n = 0; n < 10; n++) {
      try { fs.rmSync(profile, { recursive: true, force: true }); break; } catch { await sleep(200); }
    }
  }
})().catch(error => { console.error(error); process.exitCode = 1; server.close(); });
