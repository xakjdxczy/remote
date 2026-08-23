const MESH_VIEWER = "尘埃X-mesh";
const TYPE_OPEN = 1;
const TYPE_DATA = 2;
const TYPE_CLOSE = 3;
const TYPE_TUN = 4;
const MAX_PEERS = 8;
const STUN = [
  { urls: ["stun:stun.l.google.com:19302", "stun:stun.qq.com:3478", "stun:stun.miwifi.com:3478", "stun:stun.cloudflare.com:3478"] },
];

const RECONNECT_MIN_MS = 2000;
const RECONNECT_MAX_MS = 30000;

const state = {
  cfg: null,
  sig: null,
  ice: STUN,
  ping: 0,
  peers: new Map(),
  bridges: new Map(),
  dialing: [],
  clock: 0,
  lastFail: null,
};

function $(id) {
  return document.getElementById(id);
}

function log(text) {
  const el = $("mesh-log");
  if (el) el.textContent = text;
}

function useParentSignal() {
  return window.parent !== window;
}

function signalOpen() {
  return useParentSignal() ? !!state.bridged : !!(state.sig && state.sig.readyState === 1);
}

function notifyMesh(extra) {
  if (typeof dustxNotifyShell !== "function") return;
  const el = $("mesh-status");
  const password = state.cfg?.password || "";
  const payload = {
    channel: "mesh",
    status: el ? el.textContent : "",
    cls: el ? String(el.className).replace("pill", "").trim() : "",
    id: state.cfg?.device_id || "",
    idDisplay: $("mesh-id")?.textContent || "",
  };
  if (password && !/^[•·.\-\s]+$/.test(password)) payload.password = password;
  payload.links = collectLinks();
  dustxNotifyShell(Object.assign(payload, extra || {}));
}

function toneFromPhase(phase) {
  const s = String(phase || "");
  if (/失败|错误|断开|不在线|正忙/.test(s)) return "err";
  if (/后重连|不稳|来电/.test(s)) return "warn";
  if (/已接通|已连接|隧道已接通/.test(s)) return "live";
  if (/正在|呼叫|打洞|收集|打开|恢复|等待对方/.test(s)) return "progress";
  return "";
}

function icePhase(peer) {
  const ready = peer.dc && peer.dc.readyState === "open";
  if (ready) return "隧道已接通";
  if (peer.status && /后重连|失败|错误/.test(peer.status)) return peer.status;
  const ice = peer.pc ? peer.pc.iceConnectionState : "";
  const gather = peer.pc ? peer.pc.iceGatheringState : "";
  if (ice === "checking") return "正在打洞…";
  if (ice === "connected" || ice === "completed") return "路径已通，正在打开通道…";
  if (ice === "disconnected") return "通道不稳，正在恢复…";
  if (ice === "failed") return "打洞失败";
  if (gather === "gathering") return "正在收集网络路径…";
  return peer.status || "正在呼叫…";
}

function collectLinks() {
  const seen = new Set();
  const out = [];
  const push = (id, phase, extra) => {
    const pid = digits(id);
    if (!pid || seen.has(pid)) return;
    seen.add(pid);
    out.push(Object.assign({
      id: pid,
      phase,
      tone: toneFromPhase(phase),
      via: "mesh",
      ssh_user: state.cfg?.ssh_user || "",
    }, extra || {}));
  };
  state.dialing.forEach((d) => push(d.id, "正在呼叫…"));
  state.peers.forEach((p) => push(p.remoteId, icePhase(p), { port: p.localPort }));
  if (state.lastFail && !seen.has(digits(state.lastFail.id))) {
    push(state.lastFail.id, state.lastFail.phase);
  }
  return out;
}

function connectedCount() {
  let n = 0;
  state.peers.forEach((p) => {
    if (p.dc && p.dc.readyState === "open") n += 1;
  });
  return n;
}

function refreshStatus() {
  const n = connectedCount();
  const pending = state.dialing.length + [...state.peers.values()].filter((p) => !(p.dc && p.dc.readyState === "open")).length;
  if (!signalOpen()) setStatus("未上线", "");
  else if (n && !pending) setStatus(n === 1 ? "隧道已接通" : `已上线 · ${n} 路已接通`, "live");
  else if (n && pending) setStatus(`已接通 ${n} 路 · 另有连接进行中`, "progress");
  else if (state.dialing.length) setStatus("正在呼叫…", "progress");
  else if (state.peers.size) {
    const phase = icePhase([...state.peers.values()][0]);
    setStatus(phase, toneFromPhase(phase) || "progress");
  } else if (state.lastFail) setStatus(state.lastFail.phase, "err");
  else setStatus("已上线，等待对端", "online");
  renderPeers();
  renderRecents();
  ensurePeerClock();
}

function nowSec() {
  return Math.floor(Date.now() / 1000);
}

function ensurePeerClock() {
  if (state.clock) return;
  state.clock = setInterval(() => {
    if (!state.peers.size) return;
    renderPeers();
  }, 1000);
}

function recentsApi() {
  return window.dustxRecents || null;
}

async function rememberMesh(item) {
  const api = recentsApi();
  if (!api || !item || !digits(item.id)) return;
  try { await api.upsert("mesh", item); } catch { /* ignore */ }
  renderRecents();
}

async function renderRecents() {
  const box = $("mesh-recents");
  const api = recentsApi();
  if (!box || !api) return;
  let items = [];
  try {
    const doc = await api.load();
    const live = new Set([...state.peers.values()].map((p) => digits(p.remoteId)).filter(Boolean));
    items = (doc.mesh || []).filter((it) => it.id && !live.has(it.id));
  } catch { /* ignore */ }
  if (!items.length) {
    box.innerHTML = "<p class=\"hint\">还没有保存过对端。连上一次后会出现在这里。</p>";
    return;
  }
  box.innerHTML = items.map((it) => {
    const can = !!it.password;
    return `<div class="mesh-peer">
      <div>
        <strong>${formatId(it.id)}</strong>
        <span class="hint">${it.incoming && !it.password ? "曾接入" : "我连对方"}${it.local_port ? ` · 127.0.0.1:${it.local_port}` : ""}</span>
        <span class="hint">${api.formatRecord(it)}</span>
      </div>
      <div class="recent-actions">
        ${can ? `<button type="button" class="ghost" data-resume="${it.id}">继续</button>` : ""}
        <button type="button" class="ghost" data-forget="${it.id}">删除</button>
      </div>
    </div>`;
  }).join("");
}

function setStatus(text, cls) {
  const el = $("mesh-status");
  if (!el) return;
  el.textContent = text;
  el.className = `pill ${cls || ""}`;
  notifyMesh();
}

function showError(text) {
  const el = $("mesh-error");
  if (!el) return;
  el.hidden = !text;
  el.textContent = text || "";
}

function formatId(id) {
  const d = String(id || "").replace(/\D/g, "");
  if (d.length !== 9) return id || "------";
  return `${d.slice(0, 3)} ${d.slice(3, 6)} ${d.slice(6)}`;
}

function digits(id) {
  return String(id || "").replace(/\D/g, "");
}

function encodeFrame(type, streamId, payload) {
  const n = payload ? payload.byteLength : 0;
  const buf = new Uint8Array(9 + n);
  const view = new DataView(buf.buffer);
  buf[0] = type;
  view.setUint32(1, streamId);
  view.setUint32(5, n);
  if (n) buf.set(new Uint8Array(payload), 9);
  return buf;
}

function settingsFromForm() {
  const tun = $("mesh-mode-tun")?.checked;
  return {
    mode: tun ? "tun" : "tunnel",
    local_port: Number($("mesh-local-port")?.value || 2222),
    remote_port: Number($("mesh-remote-port")?.value || 22),
    ssh_user: $("mesh-ssh-user")?.value || "",
    local_ip: $("mesh-local-ip")?.value || "100.64.0.1",
    peer_ip: $("mesh-peer-ip")?.value || "100.64.0.2",
    device_id: digits(state.cfg?.device_id || ""),
  };
}

function usedPorts() {
  return new Set([...state.peers.values()].map((p) => p.localPort).filter(Boolean));
}

function findPeerByRemote(id, role) {
  const d = digits(id);
  if (!d) return null;
  for (const p of state.peers.values()) {
    if (digits(p.remoteId) === d && (!role || p.role === role)) return p;
  }
  return null;
}

function keepOneViewer(remoteId, preferred) {
  const d = digits(remoteId);
  if (!d) return preferred || null;
  let kept = preferred && digits(preferred.remoteId) === d ? preferred : null;
  for (const p of [...state.peers.values()]) {
    if (digits(p.remoteId) !== d || p.role !== "viewer") continue;
    if (!kept) {
      kept = p;
      continue;
    }
    if (p === kept) continue;
    p.want = false;
    if (p.reconnectTimer) {
      clearTimeout(p.reconnectTimer);
      p.reconnectTimer = 0;
    }
    dropPeer(p.sessionId);
  }
  return kept;
}

function allocLocalPort() {
  let p = Number($("mesh-local-port")?.value || 2222);
  const used = usedPorts();
  while (used.has(p) && p < 65000) p += 1;
  return p;
}

function pickPort(preferred) {
  const used = usedPorts();
  const p = Number(preferred || 0);
  if (p > 0 && p < 65536 && !used.has(p)) return p;
  return allocLocalPort();
}

function sshHint(cfg) {
  const ssh = cfg.ssh || {};
  const user = ssh.os === "windows" && ssh.username ? ssh.username : "对端用户名";
  const remotePort = cfg.remote_port || 22;
  if (!state.peers.size) {
    const port = cfg.local_port || 2222;
    return `你连别人时，在本机执行：ssh -p ${port} ${user}@127.0.0.1。别人连你时，对方会打到你这台的 ${remotePort} 端口（本机 OpenSSH 见右侧）。`;
  }
  return [...state.peers.values()]
    .map((p) => (p.role === "viewer"
      ? `ssh -p ${p.localPort} ${user}@127.0.0.1  →  ${formatId(p.remoteId || "")}`
      : `${formatId(p.remoteId || "")} 已连进来；对方会 ssh 到你这台的 ${remotePort} 端口`))
    .join("\n");
}

function applySsh(ssh) {
  const status = $("mesh-ssh-status");
  const btn = $("mesh-ssh-enable");
  if (!status || !btn) return;
  if (!ssh) {
    status.textContent = "";
    btn.hidden = true;
    return;
  }
  status.textContent = ssh.message || "";
  btn.hidden = !ssh.can_enable || ssh.ready;
  btn.disabled = !!ssh.busy;
  if (ssh.busy) btn.textContent = "正在开启…";
  else btn.textContent = ssh.need_admin ? "请先以管理员运行" : "开启本机 SSH";
}

function applyCfg(cfg) {
  state.cfg = cfg;
  if ($("mesh-id")) $("mesh-id").textContent = formatId(cfg.device_id) || "------";
  if ($("mesh-ssh-user") && cfg.ssh_user != null) $("mesh-ssh-user").value = cfg.ssh_user || "";
  if (cfg.local_port) $("mesh-local-port").value = cfg.local_port;
  if (cfg.remote_port) $("mesh-remote-port").value = cfg.remote_port;
  if (cfg.local_ip) $("mesh-local-ip").value = cfg.local_ip;
  if (cfg.peer_ip) $("mesh-peer-ip").value = cfg.peer_ip;
  const tunOn = cfg.mode === "tun" && cfg.tun_available;
  $("mesh-mode-tun").checked = tunOn;
  $("mesh-mode-tunnel").checked = !tunOn;
  $("mesh-mode-tun").disabled = !cfg.tun_available;
  $("mesh-mode-tun-wrap").classList.toggle("is-disabled", !cfg.tun_available);
  $("mesh-tun-hint").textContent = cfg.tun_reason || $("mesh-tun-hint").textContent;
  if (Array.isArray(cfg.ice_servers) && cfg.ice_servers.length) state.ice = cfg.ice_servers;
  $("mesh-ssh").textContent = sshHint(cfg);
  applySsh(cfg.ssh);
  notifyMesh();
}

function renderPeers() {
  const box = $("mesh-peers");
  if (!box) return;
  if (!state.peers.size) {
    box.innerHTML = "<p class=\"hint\">还没有对端。填识别码再点连接，或等别人连进来。可以同时挂多路，每路一个本地端口。</p>";
    return;
  }
  box.innerHTML = [...state.peers.values()].map((p) => {
    const ready = p.dc && p.dc.readyState === "open";
    const label = p.role === "viewer" ? "我连对方" : "对方连进来";
    const where = p.role === "viewer"
      ? `127.0.0.1:${p.localPort}`
      : `对端连你的 ${state.cfg?.remote_port || 22} 端口`;
    const phase = icePhase(p);
    const tone = toneFromPhase(phase) || (ready ? "live" : "progress");
    const start = p.connectedAt || p.startedAt || 0;
    const api = recentsApi();
    const timeLine = start && api ? api.formatLive(start) : "";
    return `<div class="mesh-peer is-${tone}">
      <div>
        <strong>${formatId(p.remoteId || "")}</strong>
        <span class="hint">${label} · ${where}</span>
        <span class="phase-tag ${tone}">${phase}</span>
        ${timeLine ? `<span class="hint">${timeLine}</span>` : ""}
      </div>
      <button type="button" class="ghost" data-hangup="${p.sessionId}">断开</button>
    </div>`;
  }).join("");
}

async function api(path, method, body) {
  const res = await fetch(path, {
    method: method || "GET",
    headers: body ? { "content-type": "application/json" } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  });
  return res.json();
}

async function loadCfg() {
  const cfg = await api("/api/mesh");
  applyCfg(cfg);
  await loadIce(cfg);
  return cfg;
}

async function saveSettings() {
  const cfg = await api("/api/mesh", "POST", settingsFromForm());
  applyCfg(cfg);
  return cfg;
}

async function loadIce(cfg) {
  const fallback = (cfg && cfg.ice_servers) || STUN;
  try {
    const origin = (cfg && cfg.signal_http) || "";
    if (!origin) {
      state.ice = fallback;
      return;
    }
    const res = await fetch(`${origin}/api/config`);
    const data = await res.json();
    if (Array.isArray(data.ice_servers) && data.ice_servers.length) state.ice = data.ice_servers;
    else state.ice = fallback;
  } catch {
    state.ice = fallback;
  }
}

function sendSig(obj, sessionId) {
  const payload = sessionId ? Object.assign({ session_id: sessionId }, obj) : obj;
  if (useParentSignal()) {
    window.parent.postMessage({ source: "dustx-sig-out", payload }, "*");
    return;
  }
  if (state.sig && state.sig.readyState === 1) state.sig.send(JSON.stringify(payload));
}

function sendToPeer(peer, buf) {
  if (peer.dc && peer.dc.readyState === "open") peer.dc.send(buf);
  else peer.pending.push(buf);
}

function flushPending(peer) {
  if (!peer.dc || peer.dc.readyState !== "open") return;
  while (peer.pending.length) peer.dc.send(peer.pending.shift());
}

function hookDc(peer, dc) {
  peer.dc = dc;
  dc.binaryType = "arraybuffer";
  dc.addEventListener("open", () => {
    flushPending(peer);
    peer.reconnectAttempt = 0;
    peer.iceRestarting = false;
    peer.status = "已接通";
    peer.connectedAt = Date.now();
    if (!peer.startedAt) peer.startedAt = peer.connectedAt;
    if (peer.remoteId) {
      rememberMesh({
        id: peer.remoteId,
        password: peer.password,
        local_port: peer.localPort,
        want: false,
        incoming: peer.role === "host",
        started_at: nowSec(),
        ended_at: 0,
      });
    }
    refreshStatus();
    detectPath(peer);
  });
  dc.addEventListener("close", () => {
    if (peer.pc) log(`${formatId(peer.remoteId)} 数据通道已关闭`);
  });
  dc.addEventListener("message", (ev) => {
    const bridge = state.bridges.get(peer.sessionId);
    if (bridge && bridge.readyState === 1) bridge.send(ev.data);
  });
}

function closePeerPc(peer) {
  if (peer.iceRestartTimer) {
    clearTimeout(peer.iceRestartTimer);
    peer.iceRestartTimer = 0;
  }
  peer.iceRestarting = false;
  try { peer.dc && peer.dc.close(); } catch { /* ignore */ }
  try { peer.pc && peer.pc.close(); } catch { /* ignore */ }
  peer.dc = null;
  peer.pc = null;
  peer.pending = [];
  peer.connectedAt = 0;
}

function closeBridge(sessionId) {
  const ws = state.bridges.get(sessionId);
  try { ws && ws.close(); } catch { /* ignore */ }
  state.bridges.delete(sessionId);
}

function otherPeerSameRemote(peer) {
  const d = digits(peer?.remoteId);
  if (!d) return false;
  for (const p of state.peers.values()) {
    if (p !== peer && digits(p.remoteId) === d) return true;
  }
  return false;
}

async function dropPeer(sessionId, reason) {
  const peer = state.peers.get(sessionId);
  if (peer) {
    if (peer.reconnectTimer) clearTimeout(peer.reconnectTimer);
    if (peer.remoteId && !otherPeerSameRemote(peer)) {
      const start = Math.floor((peer.connectedAt || peer.startedAt || Date.now()) / 1000);
      rememberMesh({
        id: peer.remoteId,
        password: peer.password,
        local_port: peer.localPort,
        want: false,
        incoming: peer.role === "host",
        started_at: start,
        ended_at: nowSec(),
      });
    }
    closePeerPc(peer);
    state.peers.delete(sessionId);
  }
  closeBridge(sessionId);
  try { await api("/api/mesh/peer/stop", "POST", { key: sessionId }); } catch { /* ignore */ }
  if (reason) log(reason);
  refreshStatus();
}

async function retargetPeer(peer, sessionId) {
  if (peer.sessionId === sessionId && state.peers.get(sessionId) === peer) return peer;
  const old = peer.sessionId;
  if (peer.reconnectTimer) {
    clearTimeout(peer.reconnectTimer);
    peer.reconnectTimer = 0;
  }
  closePeerPc(peer);
  closeBridge(old);
  state.peers.delete(old);
  try { await api("/api/mesh/peer/stop", "POST", { key: old }); } catch { /* ignore */ }
  peer.sessionId = sessionId;
  peer.status = "打洞中";
  if (!peer.startedAt) peer.startedAt = Date.now();
  state.peers.set(sessionId, peer);
  const added = await api("/api/mesh/peer", "POST", { key: sessionId, local_port: peer.localPort });
  if (added && added.ok === false) {
    state.peers.delete(sessionId);
    throw new Error(added.error || "无法监听本地端口");
  }
  if (added && added.local_port) peer.localPort = added.local_port;
  openBridge(sessionId);
  refreshStatus();
  return peer;
}

async function ensurePeer(sessionId, role, meta) {
  let peer = state.peers.get(sessionId);
  if (peer) return peer;
  const existing = role === "viewer" && meta.remoteId ? findPeerByRemote(meta.remoteId, "viewer") : null;
  if (existing) {
    if (meta.password) existing.password = meta.password;
    if (meta.want) existing.want = true;
    keepOneViewer(meta.remoteId, existing);
    return retargetPeer(existing, sessionId);
  }
  if (state.peers.size >= MAX_PEERS) throw new Error(`同时最多 ${MAX_PEERS} 路互访`);
  peer = {
    sessionId,
    role,
    remoteId: meta.remoteId || "",
    password: meta.password || "",
    localPort: pickPort(meta.localPort),
    pc: null,
    dc: null,
    pending: [],
    status: "打洞中",
    want: !!meta.want,
    reconnectTimer: 0,
    reconnectAttempt: 0,
    iceRestartTimer: 0,
    iceRestarting: false,
    startedAt: Date.now(),
    connectedAt: 0,
  };
  state.peers.set(sessionId, peer);
  const added = await api("/api/mesh/peer", "POST", { key: sessionId, local_port: peer.localPort });
  if (added && added.ok === false) {
    state.peers.delete(sessionId);
    throw new Error(added.error || "无法监听本地端口");
  }
  if (added && added.local_port) peer.localPort = added.local_port;
  openBridge(sessionId);
  if (peer.remoteId) {
    rememberMesh({
      id: peer.remoteId,
      password: peer.password,
      local_port: peer.localPort,
      want: false,
      incoming: peer.role === "host",
      started_at: nowSec(),
      ended_at: 0,
    });
  }
  refreshStatus();
  return peer;
}

function openBridge(sessionId) {
  if (state.bridges.has(sessionId) && state.bridges.get(sessionId).readyState <= 1) return;
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const path = sessionId && sessionId !== "default" ? `/mesh/bridge/${sessionId}` : "/mesh/bridge";
  const ws = new WebSocket(`${proto}://${location.host}${path}`);
  ws.binaryType = "arraybuffer";
  ws.addEventListener("message", (ev) => {
    const peer = state.peers.get(sessionId);
    if (peer) sendToPeer(peer, ev.data);
  });
  ws.addEventListener("close", () => {
    if (state.bridges.get(sessionId) === ws) state.bridges.delete(sessionId);
  });
  state.bridges.set(sessionId, ws);
}

function scheduleReconnect(peer, why) {
  if (!peer.want || !peer.password || !peer.remoteId) return;
  const kept = peer.role === "host" ? peer : keepOneViewer(peer.remoteId, peer);
  if (!kept || kept !== peer) return;
  if (peer.reconnectTimer) return;
  const n = peer.reconnectAttempt;
  const ms = Math.min(RECONNECT_MAX_MS, RECONNECT_MIN_MS * (2 ** Math.min(n, 4)));
  peer.reconnectAttempt = n + 1;
  const sec = Math.round(ms / 1000);
  peer.status = `${sec}s 后重连`;
  refreshStatus();
  if (why) log(`${formatId(peer.remoteId)} ${why}，${sec} 秒后重连`);
  peer.reconnectTimer = setTimeout(() => {
    peer.reconnectTimer = 0;
    reconnectPeer(peer).catch((e) => {
      log(String(e.message || e));
      scheduleReconnect(peer, "重连失败");
    });
  }, ms);
}

async function reconnectPeer(peer) {
  if (!peer.want) return;
  const id = digits(peer.remoteId);
  if (!id || !peer.password) return;
  if (peer.dc && peer.dc.readyState === "open") return;
  keepOneViewer(id, peer);
  if (state.dialing.some((d) => d.id === id)) return;
  if (!signalOpen()) await goOnline();
  if (!peer.want) return;
  if (!signalOpen()) {
    scheduleReconnect(peer, "信令未接通");
    return;
  }
  if (state.dialing.some((d) => d.id === id)) return;
  state.dialing.push({ id, password: peer.password, want: true, localPort: peer.localPort });
  sendSig({ type: "connect", device_id: id, password: peer.password || "", token: peer.token || "", name: MESH_VIEWER });
}

async function tryIceRestart(peer) {
  const pc = peer.pc;
  if (!pc || peer.iceRestarting || peer.role !== "viewer") return;
  if (pc.iceConnectionState === "connected" || pc.iceConnectionState === "completed") return;
  peer.iceRestarting = true;
  log(`${formatId(peer.remoteId)} 通道不稳，正在 ICE restart…`);
  try {
    const offer = await pc.createOffer({ iceRestart: true });
    await pc.setLocalDescription(offer);
    await waitGathering(pc);
    if (peer.pc !== pc) return;
    sendSig({
      type: "signal",
      kind: "offer",
      sdp: { type: pc.localDescription.type, sdp: pc.localDescription.sdp },
    }, peer.sessionId);
  } catch (e) {
    log(String(e.message || e));
  } finally {
    if (peer.pc === pc) peer.iceRestarting = false;
  }
}

async function waitGathering(pc, ms) {
  if (pc.iceGatheringState === "complete") return;
  await new Promise((resolve) => {
    const t = setTimeout(resolve, ms || 8000);
    pc.addEventListener("icegatheringstatechange", () => {
      if (pc.iceGatheringState === "complete") {
        clearTimeout(t);
        resolve();
      }
    });
  });
}

async function startPc(peer, asViewer, iceServers) {
  closePeerPc(peer);
  peer.role = asViewer ? "viewer" : "host";
  const pc = new RTCPeerConnection({ iceServers: iceServers || state.ice, iceTransportPolicy: "all" });
  peer.pc = pc;
  peer.status = "正在收集网络路径…";
  refreshStatus();
  pc.addEventListener("icecandidate", (ev) => {
    if (ev.candidate) sendSig({ type: "signal", kind: "ice", candidate: ev.candidate }, peer.sessionId);
  });
  pc.addEventListener("icegatheringstatechange", () => {
    if (peer.pc !== pc) return;
    if (pc.iceGatheringState === "gathering") {
      peer.status = "正在收集网络路径…";
      refreshStatus();
    }
  });
  pc.addEventListener("iceconnectionstatechange", () => {
    if (peer.pc !== pc) return;
    const ice = pc.iceConnectionState;
    if (ice === "checking") {
      peer.status = "正在打洞…";
      refreshStatus();
      return;
    }
    if (ice === "connected" || ice === "completed") {
      peer.reconnectAttempt = 0;
      if (!(peer.dc && peer.dc.readyState === "open")) peer.status = "路径已通，正在打开通道…";
      if (peer.iceRestartTimer) {
        clearTimeout(peer.iceRestartTimer);
        peer.iceRestartTimer = 0;
      }
      refreshStatus();
      return;
    }
    if (ice === "disconnected") {
      peer.status = "通道不稳，正在恢复…";
      refreshStatus();
      log(`${formatId(peer.remoteId)} 通道不稳定，正在尝试恢复…`);
      if (peer.iceRestartTimer) clearTimeout(peer.iceRestartTimer);
      peer.iceRestartTimer = setTimeout(() => {
        peer.iceRestartTimer = 0;
        if (peer.pc === pc && pc.iceConnectionState === "disconnected") {
          tryIceRestart(peer).catch(() => {});
        }
      }, 1500);
      return;
    }
    if (ice === "failed") {
      peer.status = "打洞失败，将重连";
      refreshStatus();
      sendSig({ type: "signal", kind: "failed", message: "ice failed" }, peer.sessionId);
      sendSig({ type: "hangup", reason: "p2p_failed" }, peer.sessionId);
      log(`${formatId(peer.remoteId)} 打洞失败，将自动重连。`);
    }
  });
  pc.addEventListener("datachannel", (ev) => hookDc(peer, ev.channel));
  if (asViewer) {
    hookDc(peer, pc.createDataChannel("mesh", { ordered: true }));
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    await waitGathering(pc);
    sendSig({
      type: "signal",
      kind: "offer",
      sdp: { type: pc.localDescription.type, sdp: pc.localDescription.sdp },
    }, peer.sessionId);
  }
}

async function handleSignal(msg) {
  const sid = String(msg.session_id || "");
  if (!sid) return;
  let peer = state.peers.get(sid) || null;
  if (!peer && msg.kind === "offer" && msg.sdp) {
    peer = await ensurePeer(sid, "host", { remoteId: "" });
  }
  if (!peer) return;
  if (!peer.pc) {
    if (msg.kind === "offer" && msg.sdp) await startPc(peer, false, state.ice);
    else return;
  }
  if (msg.kind === "offer" && msg.sdp) {
    await peer.pc.setRemoteDescription(msg.sdp);
    const answer = await peer.pc.createAnswer();
    await peer.pc.setLocalDescription(answer);
    await waitGathering(peer.pc);
    sendSig({
      type: "signal",
      kind: "answer",
      sdp: { type: peer.pc.localDescription.type, sdp: peer.pc.localDescription.sdp },
    }, peer.sessionId);
  } else if (msg.kind === "answer" && msg.sdp) {
    await peer.pc.setRemoteDescription(msg.sdp);
  } else if (msg.kind === "ice" && msg.candidate) {
    try { await peer.pc.addIceCandidate(msg.candidate); } catch { /* ignore */ }
  }
}

function onSigMessage(ev) {
  if (typeof ev.data !== "string") return;
  const msg = JSON.parse(ev.data);
  if (msg.type === "registered") {
    if (useParentSignal()) state.bridged = true;
    state.cfg = Object.assign({}, state.cfg, {
      device_id: msg.device_id,
      password: msg.temp_password,
    });
    if ($("mesh-id")) $("mesh-id").textContent = msg.device_id_display || formatId(msg.device_id);
    if ($("mesh-pass") && msg.temp_password && !/^[•·.\-\s]+$/.test(msg.temp_password)) {
      $("mesh-pass").textContent = msg.temp_password;
    }
    api("/api/mesh", "POST", { device_id: digits(msg.device_id) }).catch(() => {});
    refreshStatus();
  } else if (msg.type === "password") {
    state.cfg.password = msg.temp_password;
    if ($("mesh-pass") && msg.temp_password && !/^[•·.\-\s]+$/.test(msg.temp_password)) {
      $("mesh-pass").textContent = msg.temp_password;
    }
    api("/api/mesh", "POST", { device_id: digits(state.cfg?.device_id || msg.device_id || "") }).catch(() => {});
  } else if (msg.type === "auth_failed") {
    const code = String(msg.message || "");
    const map = {
      "device offline": "对端不在线",
      "wrong password": "密码错误",
      "device busy": "对端正忙（互访已满 8 路）",
      "cannot connect to self": "不能连自己的识别码",
    };
    const text = map[code] || code || "连接失败";
    showError(text);
    const pending = state.dialing.shift();
    if (pending) state.lastFail = { id: pending.id, phase: text };
    if (pending && (code === "device busy" || code === "device offline")) {
      const existing = findPeerByRemote(pending.id, "viewer");
      if (existing) scheduleReconnect(existing, text);
    }
    refreshStatus();
  } else if (msg.type === "agent") {
    handleAgent(msg).catch((e) => {
      sendSig({ type: "agent_result", id: msg.id, ok: false, error: String(e.message || e) });
    });
  } else if (msg.type === "incoming_call") {
    const name = String(msg.viewer_name || "");
    if (!/mesh/i.test(name)) return;
    sendSig({ type: "auth_result", session_id: msg.session_id, ok: true });
    log(`对端 ${formatId(msg.viewer_id || "") || name} 正在接入`);
    setStatus("对方正在接入…", "progress");
  } else if (msg.type === "session_start") {
    if (!/mesh/i.test(String(msg.viewer_name || ""))) return;
    showError("");
    state.lastFail = null;
    if (Array.isArray(msg.ice_servers) && msg.ice_servers.length) state.ice = msg.ice_servers;
    const sid = String(msg.session_id || "");
    const iAmHost = digits(msg.host_id) === digits(state.cfg?.device_id);
    const pending = iAmHost ? null : state.dialing.shift();
    ensurePeer(sid, iAmHost ? "host" : "viewer", {
      remoteId: iAmHost ? (msg.viewer_id || "") : (msg.host_id || pending?.id || ""),
      password: pending?.password || "",
      want: !!(pending && pending.want),
      localPort: pending?.localPort,
    }).then((peer) => {
      if (pending?.want) peer.want = true;
      startPc(peer, !iAmHost, msg.ice_servers || state.ice).catch((e) => log(String(e)));
    }).catch((e) => {
      showError(String(e.message || e));
      sendSig({ type: "hangup", reason: "hangup" }, sid);
    });
  } else if (msg.type === "signal") {
    handleSignal(msg).catch((e) => log(String(e)));
  } else if (msg.type === "session_end") {
    const sid = String(msg.session_id || "");
    const reason = String(msg.reason || "");
    const peer = state.peers.get(sid);
    const auto = !!(peer && peer.want && reason !== "hangup" && reason !== "offline");
    if (auto && peer) {
      closePeerPc(peer);
      closeBridge(sid);
      try { api("/api/mesh/peer/stop", "POST", { key: sid }); } catch { /* ignore */ }
      log(reason === "p2p_failed" ? `${formatId(peer.remoteId)} 打洞失败` : (reason ? `会话结束：${reason}` : "会话结束"));
      scheduleReconnect(peer, reason === "p2p_failed" ? "打洞失败" : `会话结束：${reason}`);
    } else {
      dropPeer(sid, reason === "hangup" ? `${formatId(peer?.remoteId || "")} 已断开` : (reason ? `会话结束：${reason}` : "会话结束"));
    }
  } else if (msg.type === "replaced") {
    log("识别码在其他窗口上线，这里已顶掉。");
    goOffline();
  } else if (msg.type === "error") {
    showError(msg.message || "信令错误");
  }
}

async function goOnline() {
  showError("");
  await saveSettings();
  const started = await api("/api/mesh/start", "POST", settingsFromForm());
  applyCfg(started);
  if (started.error) {
    showError(started.error);
    return;
  }
  if (started.tun_error) log(started.tun_error);
  if (started.mode === "tun" || $("mesh-mode-tun")?.checked) openBridge("default");
  if (useParentSignal()) {
    state.bridged = true;
    refreshStatus();
    return;
  }
  if (state.sig && state.sig.readyState === 1) {
    refreshStatus();
    return;
  }
  const url = started.signal_ws || state.cfg.signal_ws;
  const ws = new WebSocket(url);
  state.sig = ws;
  ws.addEventListener("message", onSigMessage);
  ws.addEventListener("close", () => {
    if (state.sig === ws) state.sig = null;
    if (state.ping) clearInterval(state.ping);
    state.ping = 0;
    [...state.peers.keys()].forEach((sid) => {
      const peer = state.peers.get(sid);
      closePeerPc(peer);
    });
    setStatus("信令断开", "");
    state.peers.forEach((peer) => {
      if (peer.want) scheduleReconnect(peer, "信令断开");
    });
  });
  await new Promise((resolve, reject) => {
    ws.addEventListener("open", resolve, { once: true });
    ws.addEventListener("error", () => reject(new Error("无法连接信令服务器")), { once: true });
  });
  sendSig({
    type: "register",
    hostname: "DustX-mesh",
    os: navigator.platform || "desktop",
    device_id: digits(state.cfg.device_id || ""),
    temp_password: "",
    fingerprint: state.cfg.fingerprint || null,
    info: state.cfg.info || null,
  });
  state.ping = setInterval(() => {
    fetch("/api/device").then((r) => r.json()).then((info) => {
      if (state.cfg) state.cfg.info = info;
      sendSig({ type: "ping", t: Date.now(), info });
    }).catch(() => sendSig({ type: "ping", t: Date.now() }));
  }, 20000);
}

async function goOffline() {
  state.dialing = [];
  state.peers.forEach((peer) => {
    peer.want = false;
    if (peer.remoteId) recentsApi()?.setWant("mesh", peer.remoteId, false);
  });
  [...state.peers.keys()].forEach((sid) => sendSig({ type: "hangup", reason: "hangup" }, sid));
  await Promise.all([...state.peers.keys()].map((sid) => dropPeer(sid)));
  closeBridge("default");
  if (!useParentSignal()) {
    try { state.sig && state.sig.close(); } catch { /* ignore */ }
    state.sig = null;
    if (state.ping) clearInterval(state.ping);
    state.ping = 0;
  }
  await api("/api/mesh/stop", "POST");
  setStatus(useParentSignal() ? "已上线，等待对端" : "未上线", useParentSignal() ? "online" : "");
  log(useParentSignal() ? "已断开互访隧道" : "已下线");
}

async function connectPeer() {
  showError("");
  if ($("mesh-mode-tun")?.checked && state.peers.size) {
    showError("虚拟网卡同时只能一路。要连多台请改用应用层隧道。");
    return;
  }
  if (!signalOpen()) await goOnline();
  const id = digits($("mesh-peer-id")?.value || "");
  let password = $("mesh-peer-pass")?.value || "";
  if (id && !password) {
    try {
      const doc = await recentsApi()?.load();
      password = recentsApi()?.peerPassword?.(doc, id) || "";
      if (password && $("mesh-peer-pass")) $("mesh-peer-pass").value = password;
    } catch { /* ignore */ }
  }
  let token = "";
  try { token = recentsApi()?.peerToken?.(await recentsApi()?.load(), id) || ""; } catch { /* ignore */ }
  if (!id || (!password && !token)) {
    showError("请填写对端识别码和密码");
    return;
  }
  if (id === digits(state.cfg?.device_id || "")) {
    showError("不能连自己的识别码");
    return;
  }
  for (const p of state.peers.values()) {
    if (digits(p.remoteId) === id) {
      showError("已经连着这台");
      return;
    }
  }
  if (state.peers.size >= MAX_PEERS) {
    showError(`同时最多 ${MAX_PEERS} 路`);
    return;
  }
  let localPort = 0;
  try {
    const doc = await recentsApi()?.load();
    const hit = (doc?.mesh || []).find((x) => x.id === id);
    if (hit?.local_port) localPort = hit.local_port;
  } catch { /* ignore */ }
  state.lastFail = null;
  state.dialing.push({ id, password, want: true, localPort });
  rememberMesh({ id, password, want: false, local_port: localPort });
  sendSig({ type: "connect", device_id: id, password, token, name: MESH_VIEWER });
  setStatus("正在呼叫…", "progress");
}

async function connectPeerFromRecord(it) {
  const id = digits(it?.id);
  let password = it?.password || "";
  let token = it?.pair_token || it?.token || "";
  if (id && (!password || !token)) {
    try {
      const doc = await recentsApi()?.load();
      password = password || recentsApi()?.peerPassword?.(doc, id) || "";
      token = token || recentsApi()?.peerToken?.(doc, id) || "";
    } catch { /* ignore */ }
  }
  if (!id || (!password && !token)) return;
  if (id === digits(state.cfg?.device_id || "")) return;
  for (const p of state.peers.values()) {
    if (digits(p.remoteId) === id) return;
  }
  if (state.dialing.some((d) => d.id === id)) return;
  if (state.peers.size >= MAX_PEERS) return;
  if ($("mesh-mode-tun")?.checked && state.peers.size) return;
  if (!signalOpen()) await goOnline();
  if ($("mesh-peer-id")) $("mesh-peer-id").value = formatId(id);
  if ($("mesh-peer-pass")) $("mesh-peer-pass").value = password;
  state.lastFail = null;
  state.dialing.push({ id, password, want: true, localPort: it.local_port || 0 });
  rememberMesh({ id, password, pair_token: token, want: false, local_port: it.local_port || 0 });
  sendSig({ type: "connect", device_id: id, password, token, name: MESH_VIEWER });
  setStatus("正在呼叫…", "progress");
}

function hangupRemote(id) {
  const pid = digits(id);
  state.dialing = state.dialing.filter((d) => d.id !== pid);
  [...state.peers.values()].forEach((p) => {
    if (digits(p.remoteId) === pid) hangupOne(p.sessionId);
  });
}

function hangupAll() {
  state.dialing = [];
  [...state.peers.values()].forEach((p) => {
    p.want = false;
    if (p.remoteId) recentsApi()?.setWant("mesh", p.remoteId, false);
    sendSig({ type: "hangup", reason: "hangup" }, p.sessionId);
    dropPeer(p.sessionId, "已断开");
  });
  refreshStatus();
}

function hangupOne(sessionId) {
  const peer = state.peers.get(sessionId);
  if (peer) {
    peer.want = false;
    if (peer.remoteId) recentsApi()?.setWant("mesh", peer.remoteId, false);
  }
  sendSig({ type: "hangup", reason: "hangup" }, sessionId);
  dropPeer(sessionId, "已断开");
}

async function detectPath(peer) {
  if (!peer.pc) return;
  try {
    const stats = await peer.pc.getStats();
    let localType = "";
    let remoteType = "";
    const locals = new Map();
    const remotes = new Map();
    stats.forEach((r) => {
      if (r.type === "local-candidate") locals.set(r.id, r.candidateType);
      if (r.type === "remote-candidate") remotes.set(r.id, r.candidateType);
    });
    stats.forEach((r) => {
      if (r.type === "candidate-pair" && (r.state === "succeeded" || r.nominated)) {
        localType = locals.get(r.localCandidateId) || "";
        remoteType = remotes.get(r.remoteCandidateId) || "";
      }
    });
    const relay = localType === "relay" || remoteType === "relay";
    const via = relay ? "TURN 中继" : "P2P";
    if (peer.role === "host") {
      log(`${formatId(peer.remoteId)} 隧道已接通：${via}。对方会 ssh 到你这台的 ${state.cfg?.remote_port || 22} 端口；本机 OpenSSH 见右侧。`);
    } else {
      log(relay
        ? `${formatId(peer.remoteId)} 隧道已接通：TURN 中继。本机 ssh -p ${peer.localPort} …`
        : `${formatId(peer.remoteId)} 隧道已接通：P2P。本机 ssh -p ${peer.localPort} 用户名@127.0.0.1`);
    }
  } catch {
    log(`${formatId(peer.remoteId)} 隧道已接通。`);
  }
}

async function handleAgent(msg) {
  const op = String(msg.op || "");
  log(`应用协议：${op || "unknown"}`);
  let result = { ok: false, error: "agent failed" };
  try {
    result = await api("/api/agent/run", "POST", msg);
  } catch (e) {
    result = { ok: false, error: String(e.message || e) };
  }
    sendSig({
    type: "agent_result",
    id: msg.id,
    ok: !!result.ok,
    error: result.error,
    op: result.op || op,
    path: result.path,
    content: result.content,
    content_b64: result.content_b64,
    entries: result.entries,
    bytes: result.bytes,
    size: result.size,
    offset: result.offset,
    cwd: result.cwd,
    exit: result.exit,
    stdout: result.stdout,
    stderr: result.stderr,
  });
}

async function enableSsh() {
  const btn = $("mesh-ssh-enable");
  const status = $("mesh-ssh-status");
  if (btn) {
    btn.disabled = true;
    btn.textContent = "正在开启…";
  }
  if (status) status.textContent = "正在开启本机 SSH，进度见下方运行日志。";
  try {
    const data = await api("/api/ssh/enable", "POST");
    if (state.cfg) state.cfg.ssh = data;
    applySsh(data);
    const started = Date.now();
    while (Date.now() - started < 10 * 60 * 1000) {
      await new Promise((r) => setTimeout(r, 1000));
      const cfg = await api("/api/mesh");
      applyCfg(cfg);
      if (window.refreshLogs) window.refreshLogs();
      if (!cfg.ssh || !cfg.ssh.busy) {
        if (cfg.ssh && cfg.ssh.ok === false) showError(cfg.ssh.message || "开启 SSH 失败");
        else showError("");
        return;
      }
    }
    showError("开启 SSH 超时，请看下方运行日志。");
  } catch (e) {
    showError(String(e.message || e));
    if (btn) {
      btn.disabled = false;
      btn.textContent = "开启本机 SSH";
    }
  }
}

function bind() {
  $("mesh-connect")?.addEventListener("click", () => connectPeer().catch((e) => showError(String(e.message || e))));
  $("mesh-hangup")?.addEventListener("click", hangupAll);
  $("mesh-peer-id")?.addEventListener("change", async () => {
    const id = digits($("mesh-peer-id")?.value || "");
    if (!id || ($("mesh-peer-pass")?.value || "")) return;
    try {
      const doc = await recentsApi()?.load();
      const password = recentsApi()?.peerPassword?.(doc, id) || "";
      if (password && $("mesh-peer-pass")) $("mesh-peer-pass").value = password;
    } catch { /* ignore */ }
  });
  window.dustxUi?.bindSecretInput($("mesh-peer-pass"), $("mesh-peer-peek"));
  window.dustxUi?.bindIdCombo($("mesh-id-combo"), {
    loadItems: async () => {
      const doc = await recentsApi()?.load();
      const seen = new Set();
      const rows = [];
      [...(doc?.mesh || []), ...(doc?.remote || [])].forEach((it) => {
        if (!it.id || seen.has(it.id)) return;
        const password = recentsApi()?.peerPassword?.(doc, it.id) || it.password;
        if (!password) return;
        seen.add(it.id);
        rows.push({
          id: it.id,
          password,
          hint: it.incoming && !it.want ? "曾接入" : (window.dustxRecents?.formatRecord(it) || "历史连接"),
        });
      });
      return rows;
    },
    onPick: (it) => {
      if ($("mesh-peer-id")) $("mesh-peer-id").value = formatId(it.id);
      if ($("mesh-peer-pass")) $("mesh-peer-pass").value = it.password || "";
    },
  });
  $("mesh-ssh-enable")?.addEventListener("click", () => enableSsh().catch((e) => showError(String(e.message || e))));
  $("mesh-peers")?.addEventListener("click", (ev) => {
    const btn = ev.target.closest("[data-hangup]");
    if (btn) hangupOne(btn.getAttribute("data-hangup"));
  });
  $("mesh-recents")?.addEventListener("click", (ev) => {
    const resume = ev.target.closest("[data-resume]");
    const forget = ev.target.closest("[data-forget]");
    if (resume) {
      recentsApi()?.load().then((doc) => {
        const it = (doc.mesh || []).find((x) => x.id === resume.getAttribute("data-resume"));
        if (it) connectPeerFromRecord(it).catch((e) => showError(String(e.message || e)));
      }).catch(() => {});
    } else if (forget) {
      recentsApi()?.forget("mesh", forget.getAttribute("data-forget")).then(() => renderRecents()).catch(() => {});
    }
  });
  window.addEventListener("message", (ev) => {
    const msg = ev.data;
    if (msg && msg.source === "dustx-sig-in" && msg.msg) {
      onSigMessage({ data: JSON.stringify(msg.msg) });
      return;
    }
    if (!msg || msg.source !== "dustx-shell") return;
    if (msg.action === "hangup-mesh") {
      hangupRemote(msg.id);
      return;
    }
    if (msg.action === "start-mesh" || (msg.action === "resume" && msg.kind === "mesh")) {
      connectPeerFromRecord(msg.item || {}).catch((e) => showError(String(e.message || e)));
    }
  });
  if (useParentSignal()) {
    state.bridged = true;
    window.parent.postMessage({ source: "dustx-sig-ready", channel: "mesh" }, "*");
  }
  ["mesh-mode-tunnel", "mesh-mode-tun", "mesh-local-port", "mesh-remote-port", "mesh-local-ip", "mesh-peer-ip", "mesh-ssh-user"].forEach((id) => {
    $(id)?.addEventListener("change", () => saveSettings().catch(() => {}));
  });
}

bind();
loadCfg()
  .then(async () => {
    try {
      const doc = await recentsApi()?.load();
      const last = [...(doc?.mesh || []), ...(doc?.remote || [])].find((x) => x.password);
      if (last) {
        const password = recentsApi()?.peerPassword?.(doc, last.id) || last.password;
        if ($("mesh-peer-id") && !$("mesh-peer-id").value) $("mesh-peer-id").value = formatId(last.id);
        if ($("mesh-peer-pass") && !$("mesh-peer-pass").value) $("mesh-peer-pass").value = password;
      }
    } catch { /* ignore */ }
    renderRecents();
    return goOnline();
  })
  .catch((e) => showError(String(e.message || e)));
