// Keyboard shortcuts only. No floating panel so it does not cover the call UI.
(function () {
  if (window.__vcamInjected) return;
  window.__vcamInjected = true;

  const send = (msg) => new Promise((resolve) => {
    chrome.runtime.sendMessage(msg, (response) => resolve(response || { ok: false, error: 'no response' }));
  });

  const isTypingTarget = (target) => {
    return target instanceof HTMLElement && !!target.closest(
      'input, textarea, select, [contenteditable="true"], [role="textbox"]'
    );
  };

  window.addEventListener('keydown', async (event) => {
    if (event.ctrlKey || event.metaKey || event.altKey) return;
    if (isTypingTarget(event.target)) return;

    const key = event.key.toLowerCase();
    const modeByDigit = { '1': 'normal', '2': 'remove', '3': 'virtualbg', '4': 'blur' };
    const mode = modeByDigit[key];

    if (mode) {
      event.preventDefault();
      event.stopPropagation();
      await send({ action: 'mode', mode });
      return;
    }

    if (key === 'b') {
      event.preventDefault();
      event.stopPropagation();
      await send({ action: 'bg' });
    }
  }, true);
})();
