const send = (msg) => new Promise((res) => chrome.runtime.sendMessage(msg, res));

async function refresh() {
  const r = await send({ action: "status" });
  const st = document.getElementById("status");
  if (!r || !r.ok) { st.textContent = "API offline (run ./run.sh)"; return; }
  const d = r.data;
  st.textContent = `mode: ${d.mode} | bg: ${d.bg_set ? "set" : "none"}`;
  document.querySelectorAll("button[data-mode]").forEach((b) => {
    b.classList.toggle("active", b.dataset.mode === d.mode);
  });
}

document.querySelectorAll("button[data-mode]").forEach((b) => {
  b.addEventListener("click", async () => {
    await send({ action: "mode", mode: b.dataset.mode });
    refresh();
  });
});
document.getElementById("bg").addEventListener("click", async () => { await send({ action: "bg" }); refresh(); });
document.getElementById("resetbg").addEventListener("click", async () => { await send({ action: "resetbg" }); refresh(); });
document.getElementById("thr").addEventListener("input", async (e) => {
  const v = parseFloat(e.target.value);
  document.getElementById("thrVal").textContent = v.toFixed(2);
  await send({ action: "threshold", value: v });
});
refresh();
setInterval(refresh, 2000);