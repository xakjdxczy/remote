const MESH_VIEWER = "尘埃X-mesh";
const TYPE_OPEN = 1;
const TYPE_DATA = 2;
const TYPE_CLOSE = 3;
const TYPE_TUN = 4;
const STUN = [
  { urls: ["stun:stun.l.google.com:19302", "stun:stun.qq.com:3478", "stun:stun.miwifi.com:3478", "stun:stun.cloudflare.com:3478"] },
];

const state = {
  cfg: null,
  sig: null,
  pc: null,
  dc: null,
  bridge: null,
  ice: STUN,
  role: "",
  pending: [],
  ping: 0,
};

function $(id) {
  return document.getElementById(id);
}

function log(text) {
  const el = $("mesh-log");
  if (el) el.textContent = text;
}

function setStatus(text, cls) {
  const el = $("mesh-status");
  if (!el) return;
  el.textContent = text;
  el.className = `pill ${cls || ""}`;
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
    local_ip: $("mesh-local-ip")?.value || "100.64.0.1",
    peer_ip: $("mesh-peer-ip")?.value || "100.64.0.2",
    device_id: digits(state.cfg?.device_id || ""),
    password: state.cfg?.password || "",
  };
}

function sshHint(cfg) {
  const port = cfg.local_port || 2222;
  const ssh = cfg.ssh || {};
  if (ssh.os === "windows" && ssh.username) {
    return `对方连上后，在对方电脑执行：ssh -p ${port} ${ssh.username}@127.0.0.1`;
  }
  return `连上后在本机终端执行：ssh -p ${port} 对端Windows用户名@127.0.0.1`;
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
  $("mesh-id").textContent = formatId(cfg.device_id) || "------";
  $("mesh-pass").textContent = cfg.password || "------";
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

function sendSig(obj) {
  if (state.sig && state.sig.readyState === 1) state.sig.send(JSON.stringify(obj));
}

function flushPending() {
  if (!state.dc || state.dc.readyState !== "open") return;
  while (state.pending.length) state.dc.send(state.pending.shift());
}

function sendToPeer(buf) {
  if (state.dc && state.dc.readyState === "open") state.dc.send(buf);
  else state.pending.push(buf);
}

function hookDc(dc) {
  state.dc = dc;
  dc.binaryType = "arraybuffer";
  dc.addEventListener("open", () => {
    flushPending();
    setStatus(state.role === "viewer" ? "已连接（主叫）" : "已连接（被叫）", "busy");
    detectPath();
  });
  dc.addEventListener("close", () => {
    if (state.pc) log("数据通道已关闭");
  });
  dc.addEventListener("message", (ev) => {
    if (state.bridge && state.bridge.readyState === 1) state.bridge.send(ev.data);
  });
}

function closeP2P() {
  try { state.dc && state.dc.close(); } catch { /* ignore */ }
  try { state.pc && state.pc.close(); } catch { /* ignore */ }
  state.dc = null;
  state.pc = null;
  state.role = "";
  state.pending = [];
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

async function startPc(asViewer, iceServers) {
  closeP2P();
  state.role = asViewer ? "viewer" : "host";
  const pc = new RTCPeerConnection({ iceServers: iceServers || state.ice, iceTransportPolicy: "all" });
  state.pc = pc;
  pc.addEventListener("icecandidate", (ev) => {
    if (ev.candidate) sendSig({ type: "signal", kind: "ice", candidate: ev.candidate });
  });
  pc.addEventListener("iceconnectionstatechange", () => {
    if (pc.iceConnectionState === "failed") {
      sendSig({ type: "signal", kind: "failed", message: "ice failed" });
      sendSig({ type: "hangup", reason: "p2p_failed" });
      log("打洞失败。若 VPS 已开 TURN，会走中继；否则请检查防火墙或配置 TURN_URLS。");
    }
  });
  pc.addEventListener("datachannel", (ev) => hookDc(ev.channel));
  if (asViewer) {
    hookDc(pc.createDataChannel("mesh", { ordered: true }));
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    await waitGathering(pc);
    sendSig({
      type: "signal",
      kind: "offer",
      sdp: { type: pc.localDescription.type, sdp: pc.localDescription.sdp },
    });
  }
}

async function handleSignal(msg) {
  if (!state.pc) {
    if (msg.kind === "offer" && msg.sdp) {
      await startPc(false, state.ice);
    } else {
      return;
    }
  }
  if (msg.kind === "offer" && msg.sdp) {
    await state.pc.setRemoteDescription(msg.sdp);
    const answer = await state.pc.createAnswer();
    await state.pc.setLocalDescription(answer);
    await waitGathering(state.pc);
    sendSig({
      type: "signal",
      kind: "answer",
      sdp: { type: state.pc.localDescription.type, sdp: state.pc.localDescription.sdp },
    });
  } else if (msg.kind === "answer" && msg.sdp) {
    await state.pc.setRemoteDescription(msg.sdp);
  } else if (msg.kind === "ice" && msg.candidate) {
    try { await state.pc.addIceCandidate(msg.candidate); } catch { /* ignore */ }
  }
}

function openBridge() {
  if (state.bridge && state.bridge.readyState <= 1) return;
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${proto}://${location.host}/mesh/bridge`);
  ws.binaryType = "arraybuffer";
  ws.addEventListener("message", (ev) => sendToPeer(ev.data));
  ws.addEventListener("close", () => {
    if (state.bridge === ws) state.bridge = null;
  });
  state.bridge = ws;
}

function closeBridge() {
  try { state.bridge && state.bridge.close(); } catch { /* ignore */ }
  state.bridge = null;
}

function onSigMessage(ev) {
  if (typeof ev.data !== "string") return;
  const msg = JSON.parse(ev.data);
  if (msg.type === "registered") {
    state.cfg = Object.assign({}, state.cfg, {
      device_id: msg.device_id,
      password: msg.temp_password,
    });
    $("mesh-id").textContent = msg.device_id_display || formatId(msg.device_id);
    $("mesh-pass").textContent = msg.temp_password;
    api("/api/mesh", "POST", settingsFromForm()).catch(() => {});
    setStatus("已上线，等待对端", "online");
  } else if (msg.type === "password") {
    state.cfg.password = msg.temp_password;
    $("mesh-pass").textContent = msg.temp_password;
    api("/api/mesh", "POST", settingsFromForm()).catch(() => {});
  } else if (msg.type === "auth_failed") {
    const map = {
      "device offline": "对端不在线",
      "wrong password": "密码错误",
      "device busy": "对端正忙（同时只能有一路互访）",
      "cannot connect to self": "不能连自己的识别码",
    };
    showError(map[msg.message] || msg.message || "连接失败");
    setStatus("已上线", "online");
  } else if (msg.type === "agent") {
    handleAgent(msg).catch((e) => {
      sendSig({ type: "agent_result", id: msg.id, ok: false, error: String(e.message || e) });
    });
  } else if (msg.type === "incoming_call") {
    const name = String(msg.viewer_name || "");
    const mesh = /mesh/i.test(name);
    sendSig({ type: "auth_result", session_id: msg.session_id, ok: mesh });
    if (!mesh) log(`已拒绝非互访来电：${name || "未知"}`);
    else log(`对端 ${name} 正在接入`);
  } else if (msg.type === "session_start") {
    showError("");
    if (Array.isArray(msg.ice_servers) && msg.ice_servers.length) state.ice = msg.ice_servers;
    if (state.role === "viewer") startPc(true, msg.ice_servers || state.ice).catch((e) => log(String(e)));
    setStatus("正在打洞…", "busy");
  } else if (msg.type === "signal") {
    handleSignal(msg).catch((e) => log(String(e)));
  } else if (msg.type === "session_end") {
    closeP2P();
    setStatus(state.sig ? "已上线" : "未上线", state.sig ? "online" : "");
    log(msg.reason === "hangup" ? "已断开" : `会话结束：${msg.reason || ""}`);
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
  openBridge();
  if (state.sig && state.sig.readyState === 1) {
    setStatus("已上线，等待对端", "online");
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
    if (state.pc) closeP2P();
    setStatus("信令断开", "");
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
    temp_password: state.cfg.password || "",
  });
  state.ping = setInterval(() => sendSig({ type: "ping", t: Date.now() }), 20000);
}

async function goOffline() {
  sendSig({ type: "hangup", reason: "offline" });
  closeP2P();
  closeBridge();
  try { state.sig && state.sig.close(); } catch { /* ignore */ }
  state.sig = null;
  if (state.ping) clearInterval(state.ping);
  state.ping = 0;
  await api("/api/mesh/stop", "POST");
  setStatus("未上线", "");
  log("已下线");
}

async function connectPeer() {
  showError("");
  if (!state.sig || state.sig.readyState !== 1) await goOnline();
  const id = digits($("mesh-peer-id")?.value || "");
  const password = $("mesh-peer-pass")?.value || "";
  if (!id || !password) {
    showError("请填写对端识别码和密码");
    return;
  }
  if (id === digits(state.cfg?.device_id || "")) {
    showError("不能连自己的识别码");
    return;
  }
  state.role = "viewer";
  sendSig({ type: "connect", device_id: id, password, name: MESH_VIEWER });
  setStatus("正在呼叫…", "busy");
}

function hangup() {
  sendSig({ type: "hangup", reason: "hangup" });
  closeP2P();
  setStatus(state.sig ? "已上线" : "未上线", state.sig ? "online" : "");
  log("已断开");
}

async function detectPath() {
  if (!state.pc) return;
  try {
    const stats = await state.pc.getStats();
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
    log(relay
      ? "通道已接通：TURN 中继（打洞失败时的回退，和 Tailscale DERP 同类）。"
      : "通道已接通：P2P 直连。本机监听已开，可用上面的 ssh 命令连对端。");
  } catch {
    log("通道已接通。");
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
    entries: result.entries,
    bytes: result.bytes,
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
  $("mesh-online")?.addEventListener("click", () => goOnline().catch((e) => showError(String(e.message || e))));
  $("mesh-offline")?.addEventListener("click", () => goOffline().catch((e) => showError(String(e.message || e))));
  $("mesh-connect")?.addEventListener("click", () => connectPeer().catch((e) => showError(String(e.message || e))));
  $("mesh-hangup")?.addEventListener("click", hangup);
  $("mesh-refresh-pass")?.addEventListener("click", () => sendSig({ type: "refresh_password" }));
  $("mesh-ssh-enable")?.addEventListener("click", () => enableSsh().catch((e) => showError(String(e.message || e))));
  ["mesh-mode-tunnel", "mesh-mode-tun", "mesh-local-port", "mesh-remote-port", "mesh-local-ip", "mesh-peer-ip"].forEach((id) => {
    $(id)?.addEventListener("change", () => saveSettings().catch(() => {}));
  });
}

bind();
loadCfg().catch((e) => showError(String(e.message || e)));
