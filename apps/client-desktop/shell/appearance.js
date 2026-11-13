// Match native titlebar / WebView appearance to the local console preference.
// System mode must release the native override so matchMedia follows the OS.
(() => {
  if (location.protocol !== 'http:' || location.hostname !== '127.0.0.1') return;
  let sent, busy = false;
  async function sync() {
    const mode = document.documentElement?.dataset.appearanceMode || 'system';
    const scheme = document.documentElement?.dataset.theme || '';
    const key = mode + ':' + scheme;
    if (busy || key === sent) return;
    busy = true;
    try { await window.__TAURI_INTERNALS__.invoke('desktop_set_theme', { mode }); sent = key; }
    catch (_) { /* Retry after the supervised origin / IPC is ready. */ }
    finally { busy = false; }
  }
  function start() {
    new MutationObserver(sync).observe(document.documentElement, { attributes:true, attributeFilter:['data-appearance-mode','data-theme'] });
    sync();
  }
  if (document.documentElement) start();
  else document.addEventListener('DOMContentLoaded', start, { once:true });
  setInterval(sync, 1000);
})();
