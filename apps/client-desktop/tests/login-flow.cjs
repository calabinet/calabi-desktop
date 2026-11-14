// node apps/client-desktop/tests/login-flow.cjs [built SPA directory]
// Isolated mock auth; no production daemon or real credentials are used.
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const os = require('node:os');
const {spawn} = require('node:child_process');
const root = path.resolve(__dirname, '../../..');
const ui = path.resolve(process.argv[2] || path.join(root,'apps/client/internal/status/ui/dist'));
const output = fs.mkdtempSync(path.join(os.tmpdir(),'calabi-login-visual-'));
const report = path.join(os.tmpdir(),'calabi-login-ui-check.json');
const now = Math.floor(Date.now()/1000);
const peers = [
 {public_key:'example-db',name:'db-box',os:'linux',allowed_ips:['100.64.0.3/32'],last_handshake_sec:now-14,rx_bytes:4414504,tx_bytes:1132462,path:'direct',rtt_micros:800},
 {public_key:'example-maya',name:'maya-mbp',os:'darwin',allowed_ips:['100.64.0.9/32'],last_handshake_sec:now-30,rx_bytes:831488,tx_bytes:98304,path:'direct',rtt_micros:23000},
 {public_key:'example-kiosk',name:'kiosk-3',os:'windows',allowed_ips:['100.64.0.18/32'],last_handshake_sec:now-40,rx_bytes:98304,tx_bytes:12288,path:'relay',relay_rtt_micros:12000,relay_region:'region-1'},
];
const me = {user:{id:1,email:'alex@example.com'},org:{id:1,name:'Acme Robotics'},plan:{code:'pro',max_tunnels:10}};
const api = {
 '/v1/console/state':{locked:false,remote:false,unlock_available:false},
 // Multiple memberships and an owned edge expose the real shell's switchers.
 '/v1/me':me, '/v1/orgs':{items:[{id:1,name:'Acme Robotics',plan_code:'pro'},{id:2,name:'Alex Workspace',plan_code:'basic'}],active_org_id:1},
 '/v1/local-token':{token:'example-only'}, '/v1/service-mode':{mode:'interactive'},
 '/v1/selfhosted':{mode:'platform',can_join:true},
 '/healthz':{state:'connected',connected:true,version:'2.8.0',server_addr:'node.acme.example:7443',uptime_seconds:5400},
 '/tunnels':{client_version:'2.8.0',active_org_id:1,edge_node_id:2,server_addr:'node.acme.example:7443',tunnels:[],uptime_seconds:5400},
 '/v1/edges':{items:[{edge_node_id:1,region:'region-1',node_label:'region-1',healthy:true,owned:false},{edge_node_id:2,region:'self-1',node_label:'Acme Node',healthy:true,owned:true}],owned_total:1},
 '/v1/edge-affinity':{prefer_platform:false},
 '/v1/tunnels':{items:[],my_total:0,team_total:0},
 '/v1/update/info':{current_version:'3.0.1',available:false},
 '/v1/mesh':{enabled:true,up:true,name:'alex-laptop',overlay:'100.64.0.7',org_id:1,coord:'coord.example:443',relay:'edge.example:3340',netmap_seen:true,peers,datapath:{},published_routes:[],subnet_aliases:[]},
 '/v1/mesh/nodes':{items:[{id:7,name:'alex-laptop',overlay:'100.64.0.7',owner_user_id:1,approved:true,online:true},...peers.map((p,i)=>({id:i+8,name:p.name,overlay:p.allowed_ips[0].split('/')[0],owner_user_id:i===1?2:1,owner_email:i===1?'maya@example.com':'alex@example.com',approved:true,online:true}))]},
 '/v1/mesh/advertise':{routes:[],advertise_exit_node:false,exit_node:'',accept_routes:false,block_incoming:false,forwarding_supported:true,alias_supported:true},
};
const files = {supported:true,available:true,preparing:false,receiving:true,assist_receiving:true,assist_supported:true,rdp_supported:true,ssh_supported:true,peers:[],sources:[],jobs:[],received:[]};
const unknown = new Set(); let authed=false; let requests=[];
const server=http.createServer((req,res)=>{
 const name=new URL(req.url,'http://localhost').pathname;
 if(name==='/v1/auth/login') {let body='';req.on('data',d=>body+=d);req.on('end',()=>{const v=JSON.parse(body);requests.push(v);let error=!['test-pass','plain-pass'].includes(v.password)?'invalid credentials':v.password==='plain-pass'?null:!v.totp_code?'totp required':v.totp_code!=='123456'?'invalid totp code':null;if(!error)authed=true;res.writeHead(error?401:200,{'Content-Type':'application/json'});res.end(JSON.stringify(error?{error}:{status:'ok'}));});return;}
 if(name==='/v1/me'&&!authed){res.writeHead(401,{'Content-Type':'application/json'});res.end(JSON.stringify({error:'unauthorized'}));return;}
 if(name==='/v1/service-mode'){res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify({mode:'interactive',console_web:'https://example.test'}));return;}
 if(api[name]){res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify(api[name]));return;}
 if(name.startsWith('/v1/')||name==='/file-service/available'){unknown.add(name);res.writeHead(200,{'Content-Type':'application/json'});res.end('{}');return;}
 const f=path.resolve(ui,'.'+decodeURIComponent(name==='/'?'/index.html':name));
 if(!f.startsWith(ui+path.sep)||!fs.existsSync(f)||!fs.statSync(f).isFile()){res.writeHead(404);res.end();return;}
 const mime={'.html':'text/html','.js':'text/javascript','.css':'text/css','.svg':'image/svg+xml','.woff2':'font/woff2','.png':'image/png'};
 res.writeHead(200,{'Content-Type':mime[path.extname(f)]||'application/octet-stream'});res.end(fs.readFileSync(f));
});
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
(async()=>{
 await new Promise(r=>server.listen(0,'127.0.0.1',r));const base='http://127.0.0.1:'+server.address().port;
 const profile=fs.mkdtempSync(path.join(os.tmpdir(),'calabi-desktop-draw-'));
 const chrome=spawn('C:/Program Files/Google/Chrome/Application/chrome.exe',['--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check','--remote-debugging-port=0','--user-data-dir='+profile],{windowsHide:true,stdio:'ignore'});
 let socket;
 try{
  for(let i=0;i<100&&!fs.existsSync(path.join(profile,'DevToolsActivePort'));i++)await sleep(100);
  const port=fs.readFileSync(path.join(profile,'DevToolsActivePort'),'utf8').split('\n')[0];const tab=await(await fetch('http://127.0.0.1:'+port+'/json/new?about:blank',{method:'PUT'})).json();socket=new WebSocket(tab.webSocketDebuggerUrl);await new Promise(r=>socket.addEventListener('open',r,{once:true}));let id=0;const pending=new Map();
  socket.addEventListener('message',e=>{const m=JSON.parse(e.data);if(pending.has(m.id)){const [r,j]=pending.get(m.id);pending.delete(m.id);m.error?j(new Error(JSON.stringify(m.error))):r(m.result)}});
  const call=(method,params={})=>new Promise((r,j)=>{const n=++id;pending.set(n,[r,j]);socket.send(JSON.stringify({id:n,method,params}))});
  const evaluate=async expression=>{const x=await call('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(x.exceptionDetails)throw new Error(JSON.stringify(x.exceptionDetails));return x.result.value};
  await call('Page.enable');await call('Emulation.setDeviceMetricsOverride',{width:1100,height:720,deviceScaleFactor:2,mobile:false});fs.mkdirSync(output,{recursive:true});const results=[];
  const waitFor=async expression=>{for(let i=0;i<100;i++){if(await evaluate(expression))return;await sleep(100)}throw new Error('Timed out: '+expression)};
  const fill=async (selector,value)=>evaluate(`(()=>{const e=document.querySelector(${JSON.stringify(selector)});if(!e)throw new Error('missing input');Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set.call(e,${JSON.stringify(value)});e.dispatchEvent(new Event('input',{bubbles:true}));})()`);
  const submit=()=>evaluate(`document.querySelector('button[type=submit]').click()`);
  for(const lang of ['en','zh-CN','zh-TW','de','es','fr','it','ja','ko','pt'])for(const theme of ['light','dark']){
   authed=false;requests=[];
   await call('Emulation.setDeviceMetricsOverride',{width:420,height:600,deviceScaleFactor:1,mobile:false});
   const init=`localStorage.clear();localStorage.setItem('calabi_lang',${JSON.stringify(lang)});localStorage.setItem('i18nextLng',${JSON.stringify(lang)});localStorage.setItem('calabi.client.theme',${JSON.stringify(theme)});window.__loginSizes=[];window.__TAURI_INTERNALS__={invoke:async(command,args)=>{if(command==='desktop_set_login_window'){window.__loginSizes.push(args);return;}return command==='product_info'?{product:'desktop',version:'3.0.1',bundled_core_version:'2.8.0'}:${JSON.stringify(files)}}};`;
   const {identifier}=await call('Page.addScriptToEvaluateOnNewDocument',{source:init});await call('Page.navigate',{url:base+'/?lang='+lang+'&appearance='+theme+'#/login'});
   await waitFor(`!!document.querySelector('#email')&&window.__loginSizes.some(x=>x.compact)`);
   await fill('#email','test@example.com');await fill('#password','bad-pass');await submit();
   await waitFor(`!!document.querySelector('.ant-alert-error')`);
   if(await evaluate(`!!document.querySelector('#totp_code')`))throw new Error('wrong password opened MFA');
   await fill('#password','test-pass');await submit();await waitFor(`!!document.querySelector('#totp_code')`);
   if(await evaluate(`!!document.querySelector('#email')||!!document.querySelector('#password')||!!document.querySelector('.ant-alert')`))throw new Error('MFA still contains credentials or required-code warning');
   if(!await evaluate(`document.activeElement.id==='totp_code'`))throw new Error('MFA focus');
   await fill('#totp_code','111111');await submit();await waitFor(`!!document.querySelector('.ant-alert-error')`);
   if(!await evaluate(`!!document.querySelector('#totp_code')&&!document.querySelector('#password')`))throw new Error('invalid MFA changed step');
   await evaluate(`document.querySelector('button .anticon-arrow-left').closest('button').click()`);await waitFor(`!!document.querySelector('#email')`);
   if(await evaluate(`!!document.querySelector('.ant-alert')||!!document.querySelector('#totp_code')`))throw new Error('back did not reset');
   await submit();await waitFor(`!!document.querySelector('#totp_code')`);
   if(!await evaluate(`window.__loginSizes.length===1&&window.__loginSizes[0].compact&&!('height' in window.__loginSizes[0])`))throw new Error('Login steps changed window geometry');
   const state=await evaluate(`({theme:document.documentElement.dataset.theme,over:document.documentElement.scrollWidth>420,vertical:document.documentElement.scrollHeight>innerHeight,passwordFields:document.querySelectorAll('input[type=password]').length,sizes:window.__loginSizes,missingText:/login\.(totpTitle|totpSub|totpSubmit|back)/.test(document.body.innerText)})`);
   if(state.over||state.vertical||state.passwordFields||state.missingText)throw new Error(JSON.stringify(state));
   const shot=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});fs.writeFileSync(path.join(output,`totp-${lang}-${theme}.png`),Buffer.from(shot.data,'base64'));
   await fill('#totp_code','123456');await submit();await waitFor(`location.hash.includes('overview')&&window.__loginSizes.some(x=>!x.compact)`);
   const last=requests.at(-1);if(last.identifier!=='test@example.com'||last.password!=='test-pass'||last.totp_code!=='123456')throw new Error('MFA resubmit lost credentials');
   if(await evaluate(`Object.values(localStorage).some(v=>v.includes('test-pass'))||Object.values(sessionStorage).some(v=>v.includes('test-pass'))`))throw new Error('persisted password');
   const twoFactorRequests=requests.length;authed=false;
   await call('Page.navigate',{url:base+'/?lang='+lang+'&appearance='+theme+'#/login'});
   await waitFor(`!!document.querySelector('#email')&&window.__loginSizes.some(x=>x.compact)`);
   await fill('#email','test@example.com');await fill('#password','plain-pass');await submit();
   await waitFor(`location.hash.includes('overview')&&window.__loginSizes.some(x=>!x.compact)`);
   results.push({lang,theme,passed:true,loginRequests:twoFactorRequests,withoutTwoFactor:true,...state});await call('Page.removeScriptToEvaluateOnNewDocument',{identifier});
  }
  fs.writeFileSync(report,JSON.stringify({viewport:{width:1100,height:720,dpr:2},results,unknown:[...unknown]},null,2)+'\n');console.log(JSON.stringify({scenarios:results.length,compactWidth:420,report,unknown:[...unknown]}));
 }finally{if(socket){socket.send(JSON.stringify({id:999999,method:'Browser.close'}));await sleep(1000);socket.close()}chrome.kill();await sleep(300);try{fs.rmSync(profile,{recursive:true,force:true});fs.rmSync(output,{recursive:true,force:true})}catch{}server.close()}
})().catch(e=>{console.error(e);process.exitCode=1;server.close()});
