const tabsEl = document.getElementById("tabs");
const stage = document.getElementById("stage");
const tabs = new Map();
let active = "";

function formatId(id) {
  const d = String(id || "").replace(/\D/g, "");
  if (d.length !== 9) return id || "------";
  return `${d.slice(0, 3)} ${d.slice(3, 6)} ${d.slice(6)}`;
}

function qs(obj) {
  return Object.entries(obj)
    .filter(([, v]) => v !== undefined && v !== null && v !== "")
    .map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v)}`)
    .join("&");
}

function renderTabs() {
  if (!tabs.size) {
    tabsEl.innerHTML = "";
    stage.innerHTML = "<p class=\"empty\">还没有屏幕。关标签会断开对应远程控制。</p>";
    return;
  }
  tabsEl.innerHTML = [...tabs.values()].map((t) => `
    <div class="tab${t.id === active ? " is-on" : ""}" data-id="${t.id}">
      <span>${formatId(t.id)}</span>
      <button type="button" data-close="${t.id}" title="关闭并断开">×</button>
    </div>
  `).join("");
  [...stage.querySelectorAll("iframe")].forEach((f) => {
    f.classList.toggle("is-off", f.dataset.id !== active);
  });
}

function openTab(item) {
  const id = String(item.id || "").replace(/\D/g, "");
  if (!id) return;
  if (tabs.has(id)) {
    active = id;
    renderTabs();
    return;
  }
  if (!tabs.size) stage.innerHTML = "";
  const url = `/remote.html?embed=1&viewer=1&${qs({
    id,
    password: item.password || "",
    token: item.token || "",
    from: item.from || "",
  })}`;
  const frame = document.createElement("iframe");
  frame.src = url;
  frame.dataset.id = id;
  frame.allow = "display-capture; camera; microphone; clipboard-read; clipboard-write; autoplay";
  stage.appendChild(frame);
  tabs.set(id, { id, frame });
  active = id;
  renderTabs();
}

function closeTab(id) {
  const tab = tabs.get(id);
  if (!tab) return;
  try { tab.frame.remove(); } catch { /* ignore */ }
  tabs.delete(id);
  if (active === id) active = tabs.size ? [...tabs.keys()][0] : "";
  renderTabs();
}

tabsEl.addEventListener("click", (ev) => {
  const close = ev.target.closest("[data-close]");
  if (close) {
    ev.stopPropagation();
    closeTab(close.getAttribute("data-close"));
    return;
  }
  const tab = ev.target.closest("[data-id]");
  if (tab) {
    active = tab.getAttribute("data-id");
    renderTabs();
  }
});

window.addEventListener("message", (ev) => {
  const msg = ev.data;
  if (!msg || msg.source !== "dustx-shell") return;
  if (msg.action === "open-remote") openTab(msg.item || {});
  if (msg.action === "close-remote") closeTab(String(msg.id || "").replace(/\D/g, ""));
});

window.addEventListener("beforeunload", () => {
  [...tabs.keys()].forEach(closeTab);
});

if (window.opener && !window.opener.closed) {
  window.opener.postMessage({ source: "dustx-remote-desk", action: "ready" }, "*");
}

renderTabs();
