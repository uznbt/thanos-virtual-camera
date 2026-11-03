const API = "http://127.0.0.1:8765";

async function api(path, method = "GET", body = null) {
  const opts = { method, headers: { "Content-Type": "application/json" } };
  if (body) opts.body = JSON.stringify(body);
  const r = await fetch(API + path, opts);
  return r.json();
}

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  (async () => {
    try {
      let data;
      switch (msg.action) {
        case "status": data = await api("/status"); break;
        case "modes": data = await api("/modes"); break;
        case "mode": data = await api("/mode", "POST", { mode: msg.mode }); break;
        case "snap": data = await api("/snap", "POST"); break;
        case "bg": data = await api("/bg", "POST"); break;
        case "resetbg": data = await api("/resetbg", "POST"); break;
        case "threshold": data = await api("/threshold", "POST", { value: msg.value }); break;
        default: data = { error: "unknown action" };
      }
      sendResponse({ ok: true, data });
    } catch (e) {
      sendResponse({ ok: false, error: String(e) });
    }
  })();
  return true; // async
});