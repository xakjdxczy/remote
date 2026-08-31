const state = {
  panel: "home",
  ch: {},
  devices: [],
  selected: "self",
  presence: {},
  signalHttp: "",
  signalWs: "",
  selfInfo: {},
  sig: null,
  sigPing: 0,
  sigRoute: {},
  sigReady: { remote: false, mesh: false },
  sigInbox: { remote: [], mesh: [] },
  lastConnect: "",
  sigOutbox: [],
  updatePoll: 0,
  ownPassword: "",
  links: {},
  pairs: {},
  forgotten: {},
  remoteDesk: null,
};

function isLiveStatus(msg) {
  if (!msg) return false;
  if (msg.incoming) return true;
  const s = String(msg.status || "");
  const cls = String(msg.cls || "");
  if (/路已连接|已接通|正在被控制|正在控制|正在呼叫|正在打洞|打洞中|来电|手机已接入|正在拉流|虚拟设备输出/.test(s)) {
    return true;
  }
  if (cls.includes("busy") && /连接|接通|打洞|呼叫|控制|接入|拉流|输出/.test(s)) return true;
  return false;
}

function closeCheck() {
  if (window.dustxUpdating) return { confirm: false, text: "" };
  const parts = [];
  if (isLiveStatus(state.ch.remote)) parts.push("远程控制");
  if (isLiveStatus(state.ch.mesh)) parts.push("跨网互访");
  if (isLiveStatus(state.ch.cam)) parts.push("手机摄像头");
  return { confirm: parts.length > 0, text: parts.join("、") };
}

window.dustxCloseCheck = closeCheck;

function formatId(raw) {
  const d = String(raw || "").replace(/\D/g, "").slice(0, 9);
  return d.replace(/(\d{3})(\d{0,3})(\d{0,3})/, (_, a, b, c) => [a, b, c].filter(Boolean).join(" ")) || "------";
}

function $(sel, root = document) {
  return root.querySelector(sel);
}

function $all(sel, root = document) {
  return [...root.querySelectorAll(sel)];
}

function showPanel(name) {
  state.panel = name;
  if (name === "remote" || name === "mesh") {
    const btn = $(`[data-panel="${name}"]`);
    if (btn) btn.hidden = false;
  }
  $all(".nav-btn").forEach((btn) => btn.classList.toggle("is-on", btn.dataset.panel === name));
  $all(".home, .panel").forEach((el) => {
    const id = el.id === "panel-home" ? "home" : el.id.replace("frame-", "");
    el.classList.toggle("is-on", id === name);
  });
}

function hasMeshSession() {
  if (isLiveStatus(state.ch.mesh)) return true;
  return meshLinkList().some((it) => it.phase);
}

function syncSessionTabs() {
  const remoteOn = isLiveStatus(state.ch.remote);
  const meshOn = hasMeshSession();
  const remoteBtn = $('[data-panel="remote"]');
  const meshBtn = $('[data-panel="mesh"]');
  if (remoteBtn) remoteBtn.hidden = !remoteOn;
  if (meshBtn) meshBtn.hidden = !meshOn;
  if (!remoteOn && state.panel === "remote") showPanel("home");
  if (!meshOn && state.panel === "mesh") showPanel("home");
}

function setDot(channel, cls, incoming) {
  const dot = $(`[data-dot="${channel}"]`);
  if (!dot) return;
  const tone = incoming ? "warn" : String(cls || "").replace("pill", "").trim();
  dot.className = `dot ${tone}`.trim();
}

function setMeta(channel, text) {
  const el = $(`[data-meta="${channel}"]`);
  if (el && text) el.textContent = text;
}

function versionLess(a, b) {
  const parse = (s) => {
    const out = [];
    let n = 0;
    let inNum = false;
    for (const ch of String(s || "")) {
      if (ch >= "0" && ch <= "9") {
        n = n * 10 + (ch.charCodeAt(0) - 48);
        inNum = true;
      } else if (inNum) {
        out.push(n);
        n = 0;
        inNum = false;
      }
    }
    if (inNum) out.push(n);
    return out.length ? out : [0];
  };
  const left = parse(a);
  const right = parse(b);
  const n = Math.max(left.length, right.length);
  for (let i = 0; i < n; i++) {
    const x = left[i] || 0;
    const y = right[i] || 0;
    if (x < y) return true;
    if (x > y) return false;
  }
  return false;
}

function setVersion(version) {
  const el = $("#me-ver");
  if (el && version) el.textContent = "版本 " + version;
}

function isMaskedSecret(v) {
  if (window.dustxUi && window.dustxUi.isMaskedSecret) return window.dustxUi.isMaskedSecret(v);
  return !v || /^[•·.\-\s]+$/.test(String(v));
}

function rememberOwnPassword(password) {
  if (isMaskedSecret(password)) return "";
  state.ownPassword = String(password);
  const el = $("#me-pass");
  if (el && el._setSecret) el._setSecret(state.ownPassword);
  return state.ownPassword;
}

function setMe(msg) {
  if (msg.idDisplay || msg.id) {
    const el = $("#me-id");
    if (el) el.textContent = formatId(msg.idDisplay || msg.id);
  }
  rememberOwnPassword(msg.password || msg.temp_password || "");
}

function setHome(channel, msg) {
  if (channel === "remote" || channel === "mesh") setMe(msg);
  const status = $(`[data-home-status="${channel}"]`);
  if (status && msg.status) {
    status.textContent = msg.status;
    status.className = `pill ${msg.cls || ""}`.trim();
  }
}

function showBanner(text) {
  const el = $("#banner");
  if (!el) return;
  if (!text) {
    el.hidden = true;
    el.textContent = "";
    return;
  }
  el.hidden = false;
  el.textContent = text;
}

function onStatus(msg) {
  const channel = msg.channel;
  if (!channel) return;
  const prev = state.ch[channel] || {};
  const next = Object.assign({}, prev, msg);
  if (isMaskedSecret(msg.password) && !isMaskedSecret(prev.password)) next.password = prev.password;
  if (isMaskedSecret(msg.temp_password) && !isMaskedSecret(prev.temp_password)) next.temp_password = prev.temp_password;
  state.ch[channel] = next;
  setDot(channel, msg.cls, msg.incoming);
  setMeta(channel, msg.status);
  setHome(channel, msg);
  if (Array.isArray(msg.links)) {
    Object.keys(state.links).forEach((id) => {
      if (!state.links[id]) return;
      delete state.links[id][channel];
      if (!Object.keys(state.links[id]).length) delete state.links[id];
    });
    msg.links.forEach((link) => {
      const id = String(link.id || "").replace(/\D/g, "");
      if (!id) return;
      const cur = state.links[id] || {};
      cur[channel] = { phase: link.phase || "", tone: link.tone || "progress", port: link.port || 0, ssh_user: link.ssh_user || "" };
      state.links[id] = cur;
    });
  }
  syncSessionTabs();
  if (msg.incoming && channel === "remote") {
    showBanner("有人请求远程控制本机，已切到远程控制页。");
    showPanel("remote");
  } else if (!msg.incoming && channel === "remote") {
    showBanner("");
  }
  if (msg.recentsChanged) renderHomeRecents();
  else if (channel === "remote" || channel === "mesh" || channel === "cam" || Array.isArray(msg.links)) {
    renderDeviceList();
  }
}

function kindLabel(kind) {
  if (kind === "cam") return "手机摄像头";
  if (kind === "self") return "本设备";
  return "设备";
}

function linkSummary(dev) {
  if (!dev) return { tone: "", label: "", parts: [] };
  if (dev.kind === "self") {
    const s = state.ch.remote || {};
    return { tone: s.cls || "", label: s.status || "本机", parts: [] };
  }
  if (dev.kind === "cam") {
    const s = state.ch.cam || {};
    return { tone: s.cls || "", label: s.status || "手机摄像头", parts: [] };
  }
  const L = state.links[dev.id] || {};
  const parts = [];
  if (L.mesh && L.mesh.phase) parts.push({ via: "互访", phase: L.mesh.phase, tone: L.mesh.tone || "progress", port: L.mesh.port });
  if (L.remote && L.remote.phase) parts.push({ via: "远程", phase: L.remote.phase, tone: L.remote.tone || "progress" });
  if (!parts.length) {
    const online = !!state.presence[dev.id]?.online;
    return { tone: online ? "online" : "", label: online ? "在线" : "离线", parts: [] };
  }
  const rank = { live: 4, progress: 3, warn: 3, err: 2, online: 1 };
  parts.sort((a, b) => (rank[b.tone] || 0) - (rank[a.tone] || 0));
  return {
    tone: parts[0].tone,
    label: parts.map((p) => `${p.via} · ${p.phase}`).join("；"),
    parts,
  };
}

function selectedDevice() {
  return state.devices.find((d) => d.key === state.selected) || state.devices[0];
}

function ownId() {
  return String((state.ch.remote && state.ch.remote.id) || (state.ch.mesh && state.ch.mesh.id) || "").replace(/\D/g, "");
}

function applyPairs(list) {
  const next = {};
  (list || []).forEach((it) => {
    const id = String(it.id || "").replace(/\D/g, "");
    if (!id || state.forgotten[id]) return;
    next[id] = { token: it.token || (state.pairs[id] && state.pairs[id].token) || "", role: it.role || "viewer" };
  });
  state.pairs = next;
  (list || []).forEach((it) => {
    const id = String(it.id || "").replace(/\D/g, "");
    if (!id || !it.token || state.forgotten[id]) return;
    window.dustxRecents?.upsert("mesh", { id: it.id, pair_token: it.token }).catch(() => {});
    window.dustxRecents?.upsert("remote", { id: it.id, pair_token: it.token }).catch(() => {});
  });
  renderDeviceList();
}

function pairAuth(dev) {
  const id = dev && dev.id;
  const pair = id && state.pairs[id];
  return {
    from: ownId(),
    token: (pair && pair.token) || dev.pair_token || "",
    password: (dev && dev.password) || "",
  };
}

function isPaired(dev) {
  return !!(dev && dev.id && state.pairs[dev.id]);
}

function pairDevice(id, password, token) {
  const pid = String(id || "").replace(/\D/g, "");
  if (!pid) return;
  delete state.forgotten[pid];
  sendSigOut({ type: "pair", device_id: pid, password: password || "", token: token || "" });
}

async function removeDevice(dev) {
  if (!dev || dev.kind === "self") return;
  if (dev.kind === "cam") {
    await window.dustxRecents?.forget("cam", dev.id || dev.transport);
  } else if (dev.id) {
    state.forgotten[dev.id] = true;
    delete state.pairs[dev.id];
    delete state.links[dev.id];
    delete state.presence[dev.id];
    unpairDevice(dev.id);
    $("#frame-mesh")?.contentWindow?.postMessage({ source: "dustx-shell", action: "hangup-mesh", id: dev.id }, "*");
    if (state.remoteDesk && !state.remoteDesk.closed) {
      try { state.remoteDesk.postMessage({ source: "dustx-shell", action: "close-remote", id: dev.id }, "*"); } catch { /* ignore */ }
    }
    await window.dustxRecents?.forget("remote", dev.id);
    await window.dustxRecents?.forget("mesh", dev.id);
  }
  if (state.selected === dev.key) state.selected = "self";
  await renderHomeRecents();
}

function unpairDevice(id) {
  const pid = String(id || "").replace(/\D/g, "");
  if (!pid) return;
  sendSigOut({ type: "unpair", device_id: pid });
}

function meshLinkList() {
  const links = [];
  Object.keys(state.links).forEach((id) => {
    const mesh = state.links[id] && state.links[id].mesh;
    if (mesh) links.push(Object.assign({ id }, mesh));
  });
  return links;
}

function openRemoteDesk(item) {
  if (item && item.id) state.pendingRemote = item;
  if (state.remoteDesk && !state.remoteDesk.closed) {
    state.remoteDesk.focus();
  } else {
    state.remoteDesk = openToolWindow("/remote-desk.html", "dustx-remote-desk");
  }
  if (item && item.id) {
    const send = () => {
      try {
        state.remoteDesk.postMessage({ source: "dustx-shell", action: "open-remote", item }, "*");
      } catch { /* ignore */ }
    };
    setTimeout(send, 300);
  }
}

function openToolWindow(url, name) {
  const w = window.open(url, name, "width=1100,height=740,menubar=no,toolbar=no,location=no");
  if (!w) showBanner("浏览器拦住了新窗口，请允许弹出窗口后再点一次。");
  return w;
}

function qs(obj) {
  return Object.entries(obj)
    .filter(([, v]) => v !== undefined && v !== null && v !== "")
    .map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v)}`)
    .join("&");
}

function runTool(tool) {
  const dev = selectedDevice();
  if (!dev) return;
  if (dev.kind === "self") {
    if (tool === "remote") return showPanel("remote");
    if (tool === "mesh") return showPanel("mesh");
    if (tool === "files") {
      showBanner("文件传输是在本机和对方之间拷文件，请先选一台已连接的设备。");
      return;
    }
    if (tool === "term") return openToolWindow("/term.html?local=1", "dustx-term");
    if (tool === "camera") return openToolWindow("/webcam.html", "dustx-webcam");
    return;
  }
  if (dev.kind === "cam") {
    if (tool === "camera" || tool === "remote") return resumeRecent("cam", dev.id);
    return;
  }
  if (tool === "remote") {
    if (!isPaired(dev) && !dev.password) {
      showBanner("先连接这台设备。");
      return;
    }
    const auth = pairAuth(dev);
    openRemoteDesk({ id: dev.id, password: auth.password, token: auth.token, from: auth.from });
    return;
  }
  if (tool === "mesh") {
    if (!isPaired(dev) && !dev.password) {
      showBanner("先连接这台设备。");
      return;
    }
    const frame = $("#frame-mesh");
    frame?.contentWindow?.postMessage({
      source: "dustx-shell",
      action: "start-mesh",
      item: { id: dev.id, password: pairAuth(dev).password, pair_token: pairAuth(dev).token },
    }, "*");
    showPanel("mesh");
    return;
  }
  if (tool === "camera") {
    const auth = pairAuth(dev);
    if (!auth.password && !auth.token) {
      showBanner("先连接这台设备。");
      return;
    }
    return openToolWindow(`/remote.html?embed=1&mode=camera&viewer=1&${qs({ id: dev.id, password: auth.password, token: auth.token, from: auth.from })}`, "dustx-camera");
  }
  if (tool === "files") {
    const auth = pairAuth(dev);
    if (!auth.password && !auth.token) {
      showBanner("先连接这台设备。");
      return;
    }
    return openToolWindow(`/files.html?${qs({ id: dev.id, password: auth.password, token: auth.token, from: auth.from })}`, "dustx-files");
  }
  if (tool === "term") {
    const auth = pairAuth(dev);
    if (!auth.password && !auth.token) {
      showBanner("先连接这台设备。");
      return;
    }
    return openToolWindow(`/term.html?${qs({ id: dev.id, password: auth.password, token: auth.token, from: auth.from })}`, "dustx-term");
  }
}

function syncTools() {
  const dev = selectedDevice();
  const title = $("#tool-title");
  const sub = $("#tool-sub");
  const note = $("#tool-note");
  if (!dev) {
    $all("[data-tool]").forEach((btn) => { btn.disabled = true; });
    if (title) title.textContent = "基础连接";
    if (sub) sub.textContent = "先在左边选一台设备";
    if (note) note.textContent = "";
    renderSpecs(null);
    return;
  }
  $all("[data-tool]").forEach((btn) => { btn.disabled = false; });
  const filesBtn = $('[data-tool="files"]');
  if (filesBtn && (dev.kind === "self" || dev.kind === "cam")) filesBtn.disabled = true;
  const online = dev.kind === "self" || !!state.presence[dev.id]?.online;
  const link = linkSummary(dev);
  if (title) title.textContent = "基础连接";
  if (sub) {
    sub.textContent = dev.kind === "self"
      ? (link.label || "本机功能")
      : `${link.label}${dev.kind === "cam" ? "" : ` · ${formatId(dev.id)}`}`;
  }
  const rail = $("#link-rail");
  if (rail) {
    if (link.parts && link.parts.length) {
      rail.hidden = false;
      rail.innerHTML = link.parts.map((p) => {
        const port = p.port ? ` · 127.0.0.1:${p.port}` : "";
        return `<span class="link-chip ${p.tone}"><i></i>${p.via} · ${p.phase}${port}</span>`;
      }).join("");
    } else {
      rail.hidden = true;
      rail.innerHTML = "";
    }
  }
  if (note) {
    note.textContent = dev.kind === "self"
      ? "本机功能。"
      : (isPaired(dev)
        ? "已连接。刷新对方密码也不会掉；取消后要重新输入当前密码。"
        : "还没连接。输入对方当前密码后点连接，只记住设备，不自动开屏幕或隧道。");
  }
  const box = $("#pair-box");
  const go = $("#pair-go");
  const off = $("#pair-off");
  const pass = $("#pair-pass");
  if (box && go && off) {
    const show = dev.kind !== "self" && dev.kind !== "cam";
    box.hidden = !show;
    go.hidden = !show || isPaired(dev);
    off.hidden = !show || !isPaired(dev);
    if (pass) pass.hidden = !show || isPaired(dev);
  }
  renderSpecs(dev);
}

function esc(s) {
  return String(s || "").replace(/[&<>"']/g, (c) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  }[c]));
}

function fmtBytes(n) {
  n = Number(n) || 0;
  if (n <= 0) return "—";
  const gb = n / (1024 * 1024 * 1024);
  if (gb >= 1) return `${gb >= 100 ? gb.toFixed(0) : gb.toFixed(1)} GB`;
  const mb = n / (1024 * 1024);
  if (mb >= 1) return `${mb.toFixed(0)} MB`;
  return `${Math.max(1, Math.round(n / 1024))} KB`;
}

function fmtUptime(sec) {
  sec = Number(sec) || 0;
  if (sec < 60) return "刚刚启动";
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  if (d) return `${d} 天 ${h} 小时`;
  if (h) return `${h} 小时 ${m} 分钟`;
  return `${m} 分钟`;
}

function fmtAgo(ts) {
  ts = Number(ts) || 0;
  if (!ts) return "";
  const sec = Math.max(0, Math.floor(Date.now() / 1000 - ts));
  if (sec < 20) return "刚刚";
  if (sec < 60) return `${sec} 秒前`;
  if (sec < 3600) return `${Math.floor(sec / 60)} 分钟前`;
  if (sec < 86400) return `${Math.floor(sec / 3600)} 小时前`;
  return `${Math.floor(sec / 86400)} 天前`;
}

function barClass(pct) {
  if (pct >= 90) return "bar hot";
  if (pct >= 75) return "bar warn";
  return "bar";
}

function specInfo(dev) {
  if (!dev) return {};
  if (dev.kind === "self") return state.selfInfo || {};
  return (state.presence[dev.id] && state.presence[dev.id].info) || {};
}

function deviceVersion(dev) {
  if (!dev) return "";
  if (dev.kind === "self") return String((state.selfInfo && state.selfInfo.version) || "");
  const presence = state.presence[dev.id] || {};
  return String(presence.version || (presence.info && presence.info.version) || "");
}

function renderSpecs(dev) {
  const box = $("#device-specs");
  if (!box) return;
  if (!dev || dev.kind === "cam") {
    box.innerHTML = "";
    box.hidden = true;
    return;
  }
  const presence = dev.kind === "self" ? { online: true, ip: "", last_seen: 0 } : (state.presence[dev.id] || {});
  const info = specInfo(dev);
  const rows = [];
  if (info.version) rows.push(["尘埃", info.version]);
  if (info.model) rows.push(["机型", info.model]);
  if (info.cpu) {
    const cores = info.cpu_cores ? ` · ${info.cpu_cores} 核` : "";
    const arch = info.cpu_arch ? ` · ${info.cpu_arch}` : "";
    rows.push(["CPU", `${info.cpu}${cores}${arch}`]);
  }
  if (info.ram_bytes) {
    const used = Number(info.ram_used_bytes) || 0;
    const pct = used ? Math.min(100, Math.round((100 * used) / info.ram_bytes)) : 0;
    const ram = used
      ? `${fmtBytes(info.ram_bytes)} · 已用 ${fmtBytes(used)}（${pct}%）`
      : fmtBytes(info.ram_bytes);
    rows.push(["内存", ram, used ? pct : 0]);
  }
  const disks = Array.isArray(info.disks) ? info.disks : [];
  if (disks.length) {
    const total = disks.reduce((s, d) => s + (Number(d.total) || 0), 0);
    const parts = disks.map((d) => {
      const pct = d.total ? Math.round((100 * (d.used || 0)) / d.total) : 0;
      const kind = d.kind ? ` ${String(d.kind).toUpperCase()}` : "";
      return `${d.name || d.path}: ${fmtBytes(d.used)} / ${fmtBytes(d.total)}${kind}（${pct}%）`;
    });
    rows.push(["硬盘", `${fmtBytes(total)} 合计\n${parts.join("\n")}`]);
  }
  if (info.board) rows.push(["主板", info.board]);
  if (info.gpu) rows.push(["显卡", info.gpu]);
  if (info.os) rows.push(["系统", info.os + (info.os_build ? `（${info.os_build}）` : "")]);
  if (info.hostname) rows.push(["计算机名", info.hostname]);
  if (info.uptime_sec && (dev.kind === "self" || presence.online)) {
    rows.push(["已运行", fmtUptime(info.uptime_sec)]);
  }
  const ip = presence.ip || "";
  if (ip) rows.push([presence.online ? "当前 IP" : "最后登录 IP", ip]);
  if (!presence.online && presence.last_seen) rows.push(["最后在线", fmtAgo(presence.last_seen)]);
  if (!rows.length) {
    box.hidden = false;
    box.innerHTML = `<div class="specs-head"><h3>设备信息</h3></div>
      <p class="muted">${dev.kind === "self" ? "正在读取本机硬件…" : (presence.online ? "对方还没上报硬件信息，需要新版尘埃。" : "离线时会显示上次在线记下的配置。")}</p>`;
    return;
  }
  box.hidden = false;
  const text = rows.map(([k, v]) => `${k}：${String(v).replace(/\n/g, "；")}`).join("\n");
  box.innerHTML = `<div class="specs-head">
      <h3>设备信息</h3>
      <button type="button" id="copy-specs">复制</button>
    </div>
    <dl>${rows.map(([k, v, pct]) => {
      const bar = typeof pct === "number" && pct > 0
        ? `<div class="${barClass(pct)}"><i style="width:${pct}%"></i></div>`
        : "";
      return `<dt>${esc(k)}</dt><dd>${esc(v).replace(/\n/g, "<br>")}${bar}</dd>`;
    }).join("")}</dl>`;
  $("#copy-specs")?.addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(text);
      const btn = $("#copy-specs");
      if (btn) {
        btn.textContent = "已复制";
        setTimeout(() => { btn.textContent = "复制"; }, 1200);
      }
    } catch { /* ignore */ }
  });
}

function renderDeviceList() {
  const box = $("#device-list");
  const count = $("#device-count");
  if (!box) return;
  const others = state.devices.filter((d) => d.kind !== "self");
  if (count) count.textContent = others.length ? `(${others.filter((d) => state.presence[d.id]?.online).length}/${others.length})` : "";
  if (!state.devices.length) {
    box.innerHTML = "<p class=\"kicker\">连过之后会出现在这里。</p>";
    return;
  }
  box.innerHTML = state.devices.map((d) => {
    const online = d.kind === "self" || !!state.presence[d.id]?.online;
    const host = state.presence[d.id]?.hostname || d.name || "";
    const label = d.kind === "self" ? "本设备" : (host || formatId(d.id));
    const link = linkSummary(d);
    const tone = link.tone || (online ? "online" : "");
    const extra = d.kind === "self"
      ? (link.label || "这台电脑")
      : (isPaired(d) ? `已连接 · ${link.label}` : link.label);
    const ver = deviceVersion(d);
    const tag = tone && tone !== "online"
      ? `<span class="phase-tag ${tone}">${esc(link.parts && link.parts[0] ? link.parts[0].phase : link.label)}</span>`
      : (d.kind === "self" ? "<span class=\"tag\">本设备</span>" : `<span class="kicker">${formatId(d.id)}</span>`);
    const verTag = ver ? `<span class="device-ver" title="尘埃 版本">${esc(ver)}</span>` : "";
    const del = d.kind === "self" ? "" : `<button type="button" class="device-del" data-forget-key="${esc(d.key)}" title="从本机列表删除">删除</button>`;
    return `<div class="device-row${d.key === state.selected ? " is-on" : ""}${tone ? ` is-${tone}` : ""}" data-key="${esc(d.key)}">
      <span class="live ${tone || (online ? "on" : "")}"></span>
      <span>
        <strong>${esc(label)}</strong>
        <span class="kicker">${esc(extra)}</span>
      </span>
      <span class="device-aside"><span class="device-meta">${tag}${verTag}</span>${del}</span>
    </div>`;
  }).join("");
  syncTools();
}

async function pollPresence() {
  try {
    const local = await (await fetch("/api/device")).json();
    if (local && (local.cpu || local.os || local.hostname)) state.selfInfo = local;
    if (local && local.version) setVersion(local.version);
  } catch { /* ignore */ }
  const ids = state.devices.filter((d) => d.kind !== "self" && d.kind !== "cam" && d.id).map((d) => d.id);
  if (!ids.length || !state.signalHttp) {
    renderDeviceList();
    return;
  }
  try {
    const res = await fetch(`${state.signalHttp.replace(/\/$/, "")}/api/presence`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ids }),
    });
    const data = await res.json();
    if (data && data.devices) state.presence = data.devices;
  } catch { /* signaling 还没更新时保持离线显示 */ }
  renderDeviceList();
}

async function renderHomeRecents() {
  if (!window.dustxRecents) return;
  let doc;
  try { doc = await window.dustxRecents.load(); }
  catch { return; }
  const selfName = (state.ch.remote && state.ch.remote.idDisplay) || (state.ch.mesh && state.ch.mesh.idDisplay) || "本设备";
  const rows = [{ key: "self", kind: "self", id: "", name: selfName }];
  const peers = new Map();
  const recency = (it) => Number(it.last_at || it.ended_at || it.started_at || 0) || 0;
  [...(doc.remote || []), ...(doc.mesh || [])].forEach((it) => {
    const id = String(it.id || "").replace(/\D/g, "");
    if (!id || state.forgotten[id]) return;
    const at = recency(it);
    const prev = peers.get(id) || { key: `peer:${id}`, kind: "peer", id, name: "", password: "", pair_token: "", last_at: 0 };
    const newer = at >= (prev.last_at || 0);
    if (it.password && (newer || !prev.password)) prev.password = it.password;
    if (it.pair_token && (newer || !prev.pair_token)) prev.pair_token = it.pair_token;
    if (it.name && (newer || !prev.name)) prev.name = it.name;
    prev.last_at = Math.max(prev.last_at || 0, at);
    peers.set(id, prev);
  });
  [...peers.values()].sort((a, b) => (b.last_at || 0) - (a.last_at || 0)).forEach((it) => rows.push(it));
  (doc.cam || []).forEach((it) => rows.push(Object.assign({ key: `cam:${it.id || it.transport}`, kind: "cam" }, it)));
  state.devices = rows;
  if (!state.devices.some((d) => d.key === state.selected)) state.selected = "self";
  renderDeviceList();
  pollPresence();
}

function findRecent(doc, id) {
  const lists = [...(doc.remote || []), ...(doc.mesh || []), ...(doc.cam || [])];
  const matches = lists.filter((x) => (x.id || x.transport) === id);
  if (!matches.length) return null;
  matches.sort((a, b) => (Number(b.last_at || 0) - Number(a.last_at || 0)));
  return matches[0];
}

function resumeRecent(kind, id) {
  const panel = kind === "cam" ? "cam" : kind;
  showPanel(panel);
  window.dustxRecents.load().then((doc) => {
    const list = doc[kind] || [];
    const item = list.find((x) => (x.id || x.transport) === id) || findRecent(doc, id) || { id, kind };
    const shared = window.dustxRecents.peerPassword?.(doc, id);
    if (shared) item.password = shared;
    const frame = $(`#frame-${panel}`);
    frame?.contentWindow?.postMessage({
      source: "dustx-shell",
      action: "resume",
      kind,
      item,
    }, "*");
  }).catch(() => {});
}

function postSig(name, msg) {
  if (!state.sigReady[name]) {
    (state.sigInbox[name] = state.sigInbox[name] || []).push(msg);
    return;
  }
  $(`#frame-${name}`)?.contentWindow?.postMessage({ source: "dustx-sig-in", msg }, "*");
}

function markSigReady(name) {
  state.sigReady[name] = true;
  const queued = state.sigInbox[name] || [];
  state.sigInbox[name] = [];
  queued.forEach((msg) => postSig(name, msg));
}

function hideUpdateBar() {
  const el = $("#update-bar");
  if (el) el.hidden = true;
}

function showUpdateBar(info, force) {
  const el = $("#update-bar");
  if (!el) return;
  const phase = info && info.phase ? info.phase : "";
  const busy = phase === "downloading" || phase === "applying" || phase === "restarting";
  const current = info && info.current ? info.current : "";
  const latest = info && info.latest ? info.latest : "";
  const behind = !!(latest && versionLess(current, latest));
  if ((current && latest && !behind) || (!busy && info && info.newer === false)) {
    window.dustxUpdating = false;
    hideUpdateBar();
    return;
  }
  const title = $("#update-title");
  const text = $("#update-text");
  const later = $("#update-later");
  el.hidden = false;
  el.classList.toggle("is-force", !!force && behind);
  if (title) title.textContent = force && behind ? "正在强制更新" : "发现新版本";
  const notes = info && info.notes && behind ? info.notes : "";
  let line = latest ? `当前 ${current}，最新 ${latest}` : "正在检查更新…";
  if (notes) line += `。${notes}`;
  if (phase === "downloading") line = "正在下载更新包…";
  if (phase === "applying" || phase === "restarting") line = "正在安装并重启…";
  if (phase === "error" && info.error) line = info.error;
  if (text) text.textContent = line;
  if (later) later.hidden = !!(force && behind) || busy;
}

async function applyUpdate(force) {
  try {
    const preview = await (await fetch("/api/update")).json();
    const behind = !!(preview && preview.latest && versionLess(preview.current || "", preview.latest));
    if (!preview || !preview.ok || !preview.newer || !behind || !preview.url || !preview.size) {
      window.dustxUpdating = false;
      hideUpdateBar();
      return;
    }
  } catch {
    window.dustxUpdating = false;
    hideUpdateBar();
    return;
  }
  window.dustxUpdating = true;
  window.dustxUpdatingAt = Date.now();
  showUpdateBar({ current: "", latest: "", phase: "downloading", notes: force ? "即使当前有连接也会重启。" : "" }, !!force);
  try {
    const data = await (await fetch("/api/update/apply", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ force: !!force }),
    })).json();
    if (!data.newer || data.phase === "idle") {
      window.dustxUpdating = false;
      hideUpdateBar();
      return;
    }
    showUpdateBar(data, !!(force || data.force));
    pollUpdateStatus();
  } catch {
    window.dustxUpdating = false;
    showUpdateBar({ phase: "error", error: "无法开始更新" }, !!force);
  }
}

function pollUpdateStatus() {
  if (state.updatePoll) return;
  state.updatePoll = setInterval(async () => {
    try {
      const data = await (await fetch("/api/update/status")).json();
      const behind = !!(data.latest && versionLess(data.current || "", data.latest));
      if (!data.newer || !behind || data.phase === "idle") {
        window.dustxUpdating = false;
        hideUpdateBar();
        clearInterval(state.updatePoll);
        state.updatePoll = 0;
        return;
      }
      showUpdateBar(data, !!(data.force || window.dustxUpdating));
      if (data.phase === "error") {
        window.dustxUpdating = false;
        clearInterval(state.updatePoll);
        state.updatePoll = 0;
      }
    } catch { /* ignore */ }
  }, 1000);
}

function considerRemoteUpdate(hint) {
  if (!hint) return;
  if (window.dustxUpdating) {
    const started = window.dustxUpdatingAt || 0;
    if (started && Date.now() - started < 120000) return;
    window.dustxUpdating = false;
  }
  checkUpdatePrompt();
}

async function checkUpdatePrompt() {
  try {
    const data = await (await fetch("/api/update")).json();
    if (!data || !data.ok) return;
    const behind = !!(data.latest && versionLess(data.current || "", data.latest));
    if (!behind || !data.newer) {
      window.dustxUpdating = false;
      hideUpdateBar();
      return;
    }
    if (data.force && data.url && data.size) {
      applyUpdate(true);
      return;
    }
    showUpdateBar(data, false);
  } catch { /* ignore */ }
}

function persistIdentity(msg) {
  if (!msg.device_id) return;
  rememberOwnPassword(msg.temp_password || "");
  fetch("/api/mesh", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ device_id: msg.device_id }),
  }).catch(() => {});
}

function routeSignalIn(msg) {
  if (msg.type === "update") {
    considerRemoteUpdate(msg);
    return;
  }
  if (msg.type === "pong" && msg.update) considerRemoteUpdate(msg.update);
  if (msg.type === "registered" && msg.update) considerRemoteUpdate(msg.update);
  const sid = String(msg.session_id || "");
  const meshName = /mesh/i.test(String(msg.viewer_name || ""));
  if ((msg.type === "incoming_call" || msg.type === "session_start") && sid) {
    state.sigRoute[sid] = meshName ? "mesh" : "remote";
  }
  if (msg.type === "session_end" && sid) {
    const ch = state.sigRoute[sid];
    delete state.sigRoute[sid];
    if (ch) {
      postSig(ch, msg);
      return;
    }
  }
  if (msg.type === "signal" && sid && state.sigRoute[sid]) {
    postSig(state.sigRoute[sid], msg);
    return;
  }
  if (msg.type === "incoming_call" || msg.type === "session_start") {
    postSig(meshName ? "mesh" : "remote", msg);
    return;
  }
  if (msg.type === "agent") {
    handleHostAgent(msg);
    return;
  }
  if (msg.type === "registered" || msg.type === "password") {
    persistIdentity(msg);
    const id = msg.device_id || "";
    const display = msg.device_id_display || formatId(id);
    const password = rememberOwnPassword(msg.temp_password || "") || state.ownPassword;
    if (Array.isArray(msg.pairs)) applyPairs(msg.pairs);
    onStatus({ channel: "remote", id, idDisplay: display, password, status: "在线", cls: "online" });
    onStatus({ channel: "mesh", id, idDisplay: display, password, status: "已上线，等待对端", cls: "online" });
    postSig("remote", msg);
    postSig("mesh", msg);
    flushSigOut();
    return;
  }
  if (msg.type === "replaced") {
    onStatus({ channel: "remote", status: "识别码被顶掉", cls: "" });
    onStatus({ channel: "mesh", status: "识别码被顶掉", cls: "" });
    postSig("remote", msg);
    postSig("mesh", msg);
    return;
  }
  if (msg.type === "paired") {
    const id = String(msg.device_id || "").replace(/\D/g, "");
    if (id) {
      state.pairs[id] = { token: msg.token || "", role: "viewer" };
      if (msg.token) {
        window.dustxRecents?.upsert("mesh", { id, pair_token: msg.token }).catch(() => {});
        window.dustxRecents?.upsert("remote", { id, pair_token: msg.token }).catch(() => {});
      }
    }
    showBanner("");
    renderDeviceList();
    return;
  }
  if (msg.type === "unpaired") {
    const id = String(msg.device_id || "").replace(/\D/g, "");
    if (id) delete state.pairs[id];
    renderDeviceList();
    return;
  }
  if (msg.type === "pairs") {
    applyPairs(msg.pairs || []);
    return;
  }
  if (msg.type === "auth_failed" || msg.type === "call_pending") {
    if (msg.message === "wrong password") showBanner("密码不对，或这对设备已经取消连接。");
    postSig(state.lastConnect || "remote", msg);
    if (!state.lastConnect) postSig("mesh", msg);
    return;
  }
  postSig("remote", msg);
  postSig("mesh", msg);
}

async function handleHostAgent(msg) {
  let result = { ok: false, error: "agent failed" };
  try {
    result = await (await fetch("/api/agent/run", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(msg || {}),
    })).json();
  } catch (e) {
    result = { ok: false, error: String(e && e.message ? e.message : e) };
  }
  const out = Object.assign({ type: "agent_result", id: msg && msg.id }, result);
  delete out.v;
  sendSigOut(out);
}

function sendSigOut(payload) {
  if (!payload) return;
  if (payload.type === "connect") {
    state.lastConnect = /mesh/i.test(String(payload.name || "")) ? "mesh" : "remote";
  }
  if (!state.sig || state.sig.readyState !== 1) {
    state.sigOutbox.push(payload);
    return;
  }
  state.sig.send(JSON.stringify(payload));
}

function flushSigOut() {
  const q = state.sigOutbox;
  state.sigOutbox = [];
  q.forEach((payload) => sendSigOut(payload));
}

async function startSignal() {
  let mesh = {};
  let device = {};
  let app = {};
  try { mesh = await (await fetch("/api/mesh")).json(); } catch { /* ignore */ }
  try { device = await (await fetch("/api/device")).json(); } catch { /* ignore */ }
  try { app = await (await fetch("/api/app")).json(); } catch { /* ignore */ }
  setVersion(device.version || app.version);
  state.signalHttp = app.signal_http || "";
  state.signalWs = app.signal_ws || mesh.signal_ws || "";
  if (device && (device.cpu || device.os || device.hostname)) state.selfInfo = device;
  if (mesh.device_id) {
    onStatus({
      channel: "remote",
      id: mesh.device_id,
      idDisplay: mesh.device_id,
      status: "待上线",
      cls: "",
    });
    onStatus({
      channel: "mesh",
      id: mesh.device_id,
      idDisplay: mesh.device_id,
      status: "待上线",
      cls: "",
    });
  }
  if (!state.signalWs) return;

  const waitSigReady = async () => {
    const start = Date.now();
    while (Date.now() - start < 4000) {
      if (state.sigReady.remote && state.sigReady.mesh) return;
      await new Promise((r) => setTimeout(r, 50));
    }
  };
  await waitSigReady();
  try { mesh = await (await fetch("/api/mesh")).json(); } catch { /* ignore */ }
  if (mesh.device_id) {
    onStatus({
      channel: "remote",
      id: mesh.device_id,
      idDisplay: mesh.device_id,
      status: "待上线",
      cls: "",
    });
    onStatus({
      channel: "mesh",
      id: mesh.device_id,
      idDisplay: mesh.device_id,
      status: "待上线",
      cls: "",
    });
  }

  const connect = () => {
    const ws = new WebSocket(state.signalWs);
    state.sig = ws;
    ws.addEventListener("message", (ev) => {
      if (typeof ev.data !== "string") return;
      try { routeSignalIn(JSON.parse(ev.data)); } catch { /* ignore */ }
    });
    ws.addEventListener("open", () => {
      const id = (state.ch.remote && state.ch.remote.id) || mesh.device_id || "";
      const info = state.selfInfo && (state.selfInfo.cpu || state.selfInfo.hostname) ? state.selfInfo : (device || mesh.info || null);
      ws.send(JSON.stringify({
        type: "register",
        hostname: (info && info.hostname) || "DustX",
        os: (info && info.os) || navigator.platform || "desktop",
        device_id: id,
        temp_password: "",
        fingerprint: mesh.fingerprint || null,
        info,
      }));
      if (state.sigPing) clearInterval(state.sigPing);
      state.sigPing = setInterval(() => {
        if (ws.readyState !== 1) return;
        fetch("/api/device").then((r) => r.json()).then((info) => {
          if (info && (info.cpu || info.hostname)) state.selfInfo = info;
          ws.send(JSON.stringify({ type: "ping", t: Date.now(), info }));
        }).catch(() => ws.send(JSON.stringify({ type: "ping", t: Date.now() })));
      }, 20000);
    });
    ws.addEventListener("close", () => {
      if (state.sig === ws) state.sig = null;
      if (state.sigPing) clearInterval(state.sigPing);
      state.sigPing = 0;
      onStatus({ channel: "remote", status: "信令断开", cls: "" });
      onStatus({ channel: "mesh", status: "信令断开", cls: "" });
      setTimeout(connect, 1500);
    });
  };
  connect();
}

async function hydrateOwnPassword() {
  try {
    const mesh = await (await fetch("/api/mesh")).json();
    if (mesh && mesh.device_id) setMe({ id: mesh.device_id, idDisplay: mesh.device_id });
  } catch { /* ignore */ }
}

function bindProxyToggle() {
  const el = $("use-system-proxy");
  if (!el) return;
  fetch("/api/settings").then((r) => r.json()).then((data) => {
    el.checked = !!data.use_system_proxy;
  }).catch(() => {});
  el.addEventListener("change", () => {
    fetch("/api/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ use_system_proxy: el.checked }),
    }).catch(() => {});
  });
}

async function loadApp() {
  bindProxyToggle();
  try {
    const app = await (await fetch("/api/app")).json();
    setVersion(app.version);
  } catch { /* ignore */ }
  try {
    const device = await (await fetch("/api/device")).json();
    if (device && (device.cpu || device.os || device.hostname)) state.selfInfo = device;
    if (device && device.version) setVersion(device.version);
  } catch { /* ignore */ }
  await hydrateOwnPassword();
  const remote = $("#frame-remote");
  if (remote) remote.src = "/remote.html?embed=1";
  try {
    const cam = await (await fetch("/api/cam")).json();
    onStatus({
      channel: "cam",
      id: cam.token || "",
      idDisplay: cam.token || "",
      status: "等待手机",
      cls: "",
    });
  } catch { /* ignore */ }
  renderHomeRecents();
  setInterval(pollPresence, 8000);
  startSignal();
  checkUpdatePrompt();
}

window.addEventListener("message", (ev) => {
  const msg = ev.data;
  if (!msg) return;
  if (msg.source === "dustx-sig-ready") {
    markSigReady(msg.channel === "mesh" ? "mesh" : "remote");
    return;
  }
  if (msg.source === "dustx-sig-out") {
    sendSigOut(msg.payload);
    return;
  }
  if (msg.source === "dustx-remote-desk") {
    if (msg.action === "ready" && state.pendingRemote) {
      openRemoteDesk(state.pendingRemote);
      state.pendingRemote = null;
    }
    return;
  }
  if (msg.source !== "dustx") return;
  if (msg.recentsChanged) {
    renderHomeRecents();
    return;
  }
  onStatus(msg);
});

$("#device-list")?.addEventListener("click", (ev) => {
  const del = ev.target.closest("[data-forget-key]");
  if (del) {
    ev.preventDefault();
    const key = del.getAttribute("data-forget-key");
    const dev = state.devices.find((d) => d.key === key);
    if (dev) removeDevice(dev);
    return;
  }
  const row = ev.target.closest("[data-key]");
  if (!row) return;
  state.selected = row.getAttribute("data-key");
  renderDeviceList();
});
$("#tool-grid")?.addEventListener("click", (ev) => {
  const btn = ev.target.closest("[data-tool]");
  if (!btn || btn.disabled) return;
  runTool(btn.getAttribute("data-tool"));
});

$all(".nav-btn").forEach((btn) => {
  btn.addEventListener("click", () => {
    showBanner("");
    const panel = btn.dataset.panel;
    if (panel === "remote") {
      if (isLiveStatus(state.ch.remote) && state.ch.remote.incoming) return showPanel("remote");
      openRemoteDesk();
      return;
    }
    showPanel(panel);
  });
});
$("#pair-go")?.addEventListener("click", () => {
  const dev = selectedDevice();
  if (!dev || !dev.id) return;
  pairDevice(dev.id, $("#pair-pass")?.value || "", pairAuth(dev).token);
});
$("#pair-off")?.addEventListener("click", () => {
  const dev = selectedDevice();
  if (dev && dev.id) unpairDevice(dev.id);
});
$("#add-go")?.addEventListener("click", () => {
  const id = String($("#add-id")?.value || "").replace(/\D/g, "");
  const password = $("#add-pass")?.value || "";
  if (!id || !password) {
    showBanner("请填写对方远程码和当前密码。");
    return;
  }
  window.dustxRecents?.upsert("remote", { id, password }).then(() => {
    state.selected = `peer:${id}`;
    renderHomeRecents();
    pairDevice(id, password, "");
  }).catch(() => pairDevice(id, password, ""));
});
$all("[data-open]").forEach((btn) => {
  btn.addEventListener("click", () => showPanel(btn.dataset.open));
});
$("#update-now")?.addEventListener("click", () => applyUpdate(false));
$("#update-later")?.addEventListener("click", hideUpdateBar);
$("#me-copy-id")?.addEventListener("click", async () => {
  const text = $("#me-id")?.textContent || "";
  if (!text || text.includes("-")) return;
  try {
    await navigator.clipboard.writeText(text.replace(/\s/g, ""));
    $("#me-copy-id").textContent = "已复制";
    setTimeout(() => { $("#me-copy-id").textContent = "复制"; }, 1200);
  } catch { /* ignore */ }
});
$("#me-refresh")?.addEventListener("click", () => sendSigOut({ type: "refresh_password" }));

window.dustxUi?.bindSecretText($("#me-pass"), $("#me-peek"));
async function pullOwnPassword() {
  try {
    const mesh = await (await fetch("/api/mesh")).json();
    if (mesh && mesh.device_id) setMe({ id: mesh.device_id, idDisplay: mesh.device_id });
  } catch { /* ignore */ }
}
pullOwnPassword();
setInterval(pullOwnPassword, 2000);

loadApp();
