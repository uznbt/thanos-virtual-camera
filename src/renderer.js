const preview = document.querySelector('#preview');
const loading = document.querySelector('#loading');
const backendText = document.querySelector('#backendText');
const backendDot = document.querySelector('#backendDot');
const threshold = document.querySelector('#threshold');
const thresholdValue = document.querySelector('#thresholdValue');
const modeButtons = [...document.querySelectorAll('.mode')];
const githubButton = document.querySelector('#github');

let currentMode = 'normal';
let backendOnline = false;

function setModeActive(mode) {
  currentMode = mode;
  modeButtons.forEach((button) => button.classList.toggle('active', button.dataset.mode === mode));
}

async function refreshStatus() {
  const response = await window.vcam.status();
  const connected = response?.ok && response.data;
  backendOnline = Boolean(connected);
  backendDot.className = `dot ${connected ? 'online' : 'offline'}`;
  backendText.textContent = connected ? 'Virtual camera online' : 'Camera backend unavailable';
  if (connected) {
    loading.style.display = 'none';
    setModeActive(response.data.mode || 'normal');
    threshold.value = String(response.data.threshold ?? threshold.value);
    thresholdValue.textContent = Number(threshold.value).toFixed(2);
  } else {
    loading.style.display = 'block';
    loading.textContent = 'Waiting for virtual camera backend…';
  }
}

async function refreshFrame() {
  const response = await window.vcam.frame();
  if (response?.ok && response.bytes) {
    preview.src = `data:image/jpeg;base64,${response.bytes}`;
  }
}

async function changeMode(mode) {
  const response = await window.vcam.setMode(mode);
  if (response?.ok) setModeActive(mode);
}

modeButtons.forEach((button) => button.addEventListener('click', () => changeMode(button.dataset.mode)));
document.querySelector('#captureBg').addEventListener('click', () => window.vcam.captureBackground());
document.querySelector('#resetBg').addEventListener('click', () => window.vcam.resetBackground());
threshold.addEventListener('input', () => {
  thresholdValue.textContent = Number(threshold.value).toFixed(2);
  window.vcam.setThreshold(Number(threshold.value));
});
githubButton.addEventListener('click', () => window.vcam.openExternal('https://github.com/uznbt/thanos-virtual-camera'));

window.addEventListener('keydown', (event) => {
  const tag = document.activeElement?.tagName?.toLowerCase();
  if (tag === 'input' || tag === 'textarea' || event.ctrlKey || event.altKey || event.metaKey) return;
  const modes = { '1': 'normal', '2': 'remove', '3': 'virtualbg', '4': 'blur' };
  if (modes[event.key]) changeMode(modes[event.key]);
  if (event.key.toLowerCase() === 'b') window.vcam.captureBackground();
});

setInterval(refreshStatus, 1000);
setInterval(refreshFrame, 150);
refreshStatus();
refreshFrame();
