function dustxMarkEmbed() {
  if (window.parent !== window) document.documentElement.classList.add("embed");
}

function dustxNotifyShell(payload) {
  if (window.parent === window) return;
  try {
    window.parent.postMessage(Object.assign({ source: "dustx" }, payload), "*");
  } catch {
    /* ignore */
  }
}

dustxMarkEmbed();
