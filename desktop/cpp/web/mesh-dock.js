const list = document.getElementById("list");
const opener = window.opener;

function formatId(id) {
  const d = String(id || "").replace(/\D/g, "");
  if (d.length !== 9) return id || "------";
  return `${d.slice(0, 3)} ${d.slice(3, 6)} ${d.slice(6)}`;
}

function render(links) {
  const rows = Array.isArray(links) ? links : [];
  if (!rows.length) {
    list.innerHTML = "<p class=\"hint\">还没有隧道。在概览里选中已连接的设备，再点跨网互访。</p>";
    return;
  }
  list.innerHTML = rows.map((it) => {
    const tone = it.tone || "progress";
    const port = it.port ? `127.0.0.1:${it.port}` : "";
    const user = it.ssh_user ? ` · ${it.ssh_user}` : "";
    return `<div class="row is-${tone}">
      <div>
        <strong>${formatId(it.id)}</strong>
        <div class="hint">${port || "正在分配本地端口"}${user}</div>
        <span class="phase ${tone}">${it.phase || "连接中"}</span>
      </div>
      <button type="button" data-hangup="${it.id}">断开</button>
    </div>`;
  }).join("");
}

function send(payload) {
  if (!opener || opener.closed) return;
  opener.postMessage(Object.assign({ source: "dustx-mesh-dock" }, payload), "*");
}

window.addEventListener("message", (ev) => {
  const msg = ev.data;
  if (!msg || msg.source !== "dustx-shell") return;
  if (msg.action === "mesh-links") render(msg.links);
});

list.addEventListener("click", (ev) => {
  const btn = ev.target.closest("[data-hangup]");
  if (!btn) return;
  send({ action: "hangup", id: btn.getAttribute("data-hangup") });
});

send({ action: "ready" });
setInterval(() => send({ action: "ready" }), 2000);
