// Actual SPA regression: node tools/rdp-p0/workspace_ssh_preview_test.cjs [built-ui]
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

    await call('Page.addScriptToEvaluateOnNewDocument', { source: `
      Object.defineProperty(navigator,'platform',{get:()=> 'Win32'});
      window.calls=[]; window.clock=1000000; Date.now=()=>window.clock;
      window.confirm=()=>true; window.closedSession=false;window.firstRead=true;
      window.__TAURI_INTERNALS__={invoke:async(command,args)=>{
        window.calls.push({command,...args});
        if(command==='remote_workspace') {
          if(args.request.op==='close')window.closedSession=true;
          return {active:window.closedSession?null:'linux',fullscreen:true,canMinimize:true,
            sessions:window.closedSession?[]:[{id:'linux',name:'Linux fixture',protocol:'ssh'}]};
        }
        if(command==='ssh_session') {
          if(args.request.op==='info')return {name:'Linux fixture',host:'100.64.0.2',account:'fixture',port:22};
          return null;
        }
        if(command==='ssh_read') {
          if(window.firstRead){window.firstRead=false;return Array.from(new TextEncoder().encode('\\x1b[2J\\x1b[HSSH_REAL_PREVIEW\\r\\n\\x1b[41mANSI COLOR 中文\\x1b[0m\\r\\nfixture$ '));}
          return new Promise(resolve=>window.feed=text=>resolve(Array.from(new TextEncoder().encode(text))));
        }
        return {};
      }};` });
    await call('Page.navigate', {url:`http://127.0.0.1:${server.address().port}/#/ssh-session`});
    const until=async(expression)=>{
      for(let n=0;n<120;n++){if(await evaluate(expression))return;await sleep(100);}
      throw new Error('Timed out: '+expression);
    };
    await until("window.calls?.some(c=>c.command==='ssh_session'&&c.request.op==='preview')");
    const first=await evaluate("window.calls.find(c=>c.command==='ssh_session'&&c.request.op==='preview').request.pixels");
    assert.equal(first.length,(2+192*108*4)*2,'bounded full terminal image');
    assert.ok(first.startsWith('c06c'),'192 x 108 RGBA');
    let red=0;for(let n=4;n<first.length;n+=8)if(first.slice(n,n+6)==='f58995')red++;
    assert.ok(red>0,'ANSI background colors must survive terminal capture');
    await evaluate("document.querySelector('.remote-edge').click()");
    await until("document.querySelector('.remote-thumbnail img')?.complete");
    const before=await evaluate("document.querySelector('.remote-thumbnail img').src");
    assert.ok(before.startsWith('data:image/png;base64,'),'session switcher must show an image');
    assert.equal(await evaluate("document.querySelector('.remote-thumbnail pre')!==null"),false);
    await evaluate("window.feed('\\x1b[?1049h\\x1b[2J\\x1b[HALTERNATE_SCREEN');");
    await until("document.querySelector('.remote-thumbnail img')?.src!=="+JSON.stringify(before));
    // Gallery updates locally while persistent history stays capped at ten seconds.
    assert.equal(await evaluate("window.calls.filter(c=>c.command==='ssh_session'&&c.request.op==='preview').length"),1);
    await evaluate('window.clock+=10001');
    await until("window.calls.filter(c=>c.command==='ssh_session'&&c.request.op==='preview').length===2");
    assert.notEqual(await evaluate("window.calls.filter(c=>c.command==='ssh_session'&&c.request.op==='preview').at(-1).request.pixels"),first,'alternate buffer must replace the thumbnail');
    await evaluate("document.querySelector('.remote-edge').click()");
    const width=await evaluate('innerWidth');
    await call('Input.dispatchMouseEvent',{type:'mouseMoved',x:width/2,y:1});
    await sleep(220);
    assert.equal(await evaluate("(()=>{const r=document.querySelector('.remote-fullscreen-toolbar').getBoundingClientRect();return r.y>=-1&&r.bottom>20})()"),true,'top-edge hover must reveal SSH controls');
    await evaluate("document.querySelectorAll('.remote-fullscreen-actions button')[1].click()");
    await until("window.calls.some(c=>c.command==='remote_workspace'&&c.request.op==='minimize')");
    await evaluate("document.querySelectorAll('.remote-fullscreen-actions button')[2].click()");
    await until("window.calls.some(c=>c.command==='remote_workspace'&&c.request.op==='close'&&c.request.id==='linux')");
    console.log(JSON.stringify({sshTerminalImage:true,ansiColors:true,alternateBuffer:true,historyThrottle:true,topHover:true,minimize:true,endSession:true}));
    await call('Browser.close').catch(() => {});
  } finally {
    socket?.close(); chrome.kill(); server.close();
    for (let n = 0; n < 10; n++) {
      try { fs.rmSync(profile, { recursive: true, force: true }); break; } catch { await sleep(200); }
    }
  }
})().catch(error => { console.error(error); process.exitCode = 1; server.close(); });
