const $ = (id) => document.getElementById(id);

const state = {
  ws: null,
  pc: null,
  dc: null,
  session: false,
  p2pReady: false,
  screenW: 1280,
  screenH: 720,
  frames: 0,
  lastFpsAt: Date.now(),
  lastPing: 0,
  transferSeq: 1,
  iceServers: [],
  relayEnabled: false,
  connMethod: null,
  statsTimer: null,
  lastBytes: 0,
  lastStatsAt: 0,
  sessionBytes: 0,
};

const TRAFFIC_KEY = "dustx_traffic_total_bytes";
function fmtBytes(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MB`;
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GB`;
}
function fmtSpeed(bytesPerSec) {
  const bits = bytesPerSec * 8;
  if (bits < 1e6) return `${(bits / 1e3).toFixed(0)} Kbps`;
  return `${(bits / 1e6).toFixed(2)} Mbps`;
}

function wsUrl() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  return `${proto}://${location.host}/ws`;
}

function showView(name) {
  document.querySelectorAll(".view").forEach((el) => el.classList.add("hidden"));
  const view = document.getElementById(`view-${name}`);
  if (view) view.classList.remove("hidden");
  document.querySelectorAll(".nav-item").forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.view === name);
  });
}

function setStatus(text, cls) {
  const pill = $("status-pill");
  pill.textContent = text;
  pill.className = `pill ${cls}`;
}

function addChat(who, text) {
  const log = $("chat-log");
  const line = document.createElement("div");
  line.className = "chat-line";
  line.innerHTML = `<div class="who">${escapeHtml(who)}</div><div>${escapeHtml(text)}</div>`;
  log.appendChild(line);
  log.scrollTop = log.scrollHeight;
}

function escapeHtml(s) {
  return String(s)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

function formatId(raw) {
  const d = String(raw || "").replace(/\D/g, "").slice(0, 9);
  return d.replace(/(\d{3})(\d{0,3})(\d{0,3})/, (_, a, b, c) => [a, b, c].filter(Boolean).join(" "));
}

function isRelayCandidateLine(line) {
  if (!line.startsWith("a=candidate:")) return false;
  const parts = line.split(/\s+/);
  const typ = parts.indexOf("typ");
  return typ >= 0 && parts[typ + 1] === "relay";
}

function stripRelaySdp(sdp) {
  return sdp
    .replace(/\r\n/g, "\n")
    .split("\n")
    .filter((line) => !isRelayCandidateLine(line))
    .join("\r\n") + "\r\n";
}

// Keep relay candidates only when a TURN server is configured; otherwise stay pure P2P.
function maybeStripRelay(sdp) {
  return state.relayEnabled ? sdp : stripRelaySdp(sdp);
}

// Inspect the negotiated ICE candidate pair to report P2P-direct vs TURN-relay.
async function detectConnType() {
  if (!state.pc) return;
  try {
    const stats = await state.pc.getStats();
    let pairId = null;
    let sel = null;
    stats.forEach((r) => { if (r.type === "transport" && r.selectedCandidatePairId) pairId = r.selectedCandidatePairId; });
    stats.forEach((r) => {
      if (r.type === "candidate-pair" && ((pairId && r.id === pairId) || (!pairId && r.selected))) sel = r;
    });
    if (!sel) stats.forEach((r) => { if (r.type === "candidate-pair" && r.state === "succeeded" && r.nominated) sel = r; });
    let method = "p2p";
    if (sel) {
      let local = null; let remote = null;
      stats.forEach((r) => { if (r.id === sel.localCandidateId) local = r; if (r.id === sel.remoteCandidateId) remote = r; });
      if ((local && local.candidateType === "relay") || (remote && remote.candidateType === "relay")) method = "relay";
    }
    applyConnMethod(method);
  } catch { /* ignore */ }
}

function applyConnMethod(method) {
  if (state.connMethod === method) return;
  state.connMethod = method;
  const relay = method === "relay";
  setStatus(relay ? "TURN 中继" : "P2P 直连", "busy");
  if ($("stat-path")) $("stat-path").textContent = relay ? "TURN 中继" : "P2P 直连";
  addChat("系统", relay ? "当前走 TURN 中继（经服务器转发）" : "当前 P2P 直连（不经服务器）");
  sendSession({ type: "conn_info", method });
}

function waitGathering(pc, ms = 8000) {
  if (pc.iceGatheringState === "complete") return Promise.resolve();
  return new Promise((resolve) => {
    const timer = setTimeout(resolve, ms);
    pc.addEventListener("icegatheringstatechange", () => {
      if (pc.iceGatheringState === "complete") {
        clearTimeout(timer);
        resolve();
      }
    });
  });
}

async function loadConfig() {
  const res = await fetch("/api/config");
  const cfg = await res.json();
  state.iceServers = cfg.ice_servers || [];
  state.relayEnabled = state.iceServers.some((s) =>
    (Array.isArray(s.urls) ? s.urls : [s.urls]).some((u) => String(u).startsWith("turn:") || String(u).startsWith("turns:")),
  );
  if (cfg.demo_host) {
    $("local-id").textContent = cfg.demo_host.device_id_display;
    $("local-pass").textContent = cfg.demo_host.password;
    $("remote-id").value = cfg.demo_host.device_id_display;
    $("remote-pass").value = cfg.demo_host.password;
    $("local-hint").textContent = "演示主机已在线。连接后画面走 P2P 直连，不经过信令服务器。";
    setStatus("演示主机在线", "online");
  }
}

function ensureSocket() {
  if (state.ws && state.ws.readyState <= 1) return state.ws;
  const ws = new WebSocket(wsUrl());
  ws.binaryType = "arraybuffer";
  state.ws = ws;
  ws.addEventListener("message", onMessage);
  ws.addEventListener("close", () => {
    if (state.session) endSession("信令已断开");
    state.ws = null;
  });
  return new Promise((resolve, reject) => {
    ws.addEventListener("open", () => resolve(ws), { once: true });
    ws.addEventListener("error", () => reject(new Error("无法连接信令服务器")), { once: true });
  });
}

function sendSignal(obj) {
  if (state.ws && state.ws.readyState === 1) {
    state.ws.send(JSON.stringify(obj));
  }
}

function sendSession(obj) {
  if (state.dc && state.dc.readyState === "open") {
    state.dc.send(JSON.stringify(obj));
  }
}

function onMessage(ev) {
  if (typeof ev.data !== "string") {
    return;
  }
  const msg = JSON.parse(ev.data);
  if (msg.type === "auth_failed") {
    $("connect-error").hidden = false;
    $("connect-error").textContent = msg.message === "device offline"
      ? "设备不在线"
      : msg.message === "wrong password"
        ? "密码错误"
        : msg.message === "device busy"
          ? "设备正忙"
          : msg.message || "连接失败";
    setStatus("未连接", "offline");
  } else if (msg.type === "session_start") {
    state.session = true;
    $("connect-error").hidden = true;
    $("session-title").textContent = `远程桌面 · ${msg.hostname || msg.host_id}`;
    showView("session");
    setStatus("正在建立 P2P…", "busy");
    addChat("系统", `信令已配对 ${msg.hostname || msg.host_id}，正在 P2P 直连`);
    startP2P(msg).catch((err) => {
      sendSignal({ type: "signal", kind: "failed", message: String(err) });
      sendSignal({ type: "hangup", reason: "p2p_failed" });
      endSession("P2P 直连失败");
    });
  } else if (msg.type === "session_end") {
    endSession(msg.reason || "已断开");
  } else if (msg.type === "signal") {
    handleSignal(msg);
  } else if (msg.type === "error") {
    addChat("系统", msg.message || "信令错误");
  }
}

function onSessionMessage(data) {
  if (typeof data !== "string") {
    return; // screen is a WebRTC video track now; datachannel carries no frames
  }
  const msg = JSON.parse(data);
  if (msg.type === "screen_info") {
    state.screenW = msg.width;
    state.screenH = msg.height;
    $("stat-size").textContent = `${msg.width}×${msg.height}`;
  } else if (msg.type === "ping") {
    sendSession({ type: "pong", t: msg.t }); // reply so the host can measure its RTT
  } else if (msg.type === "pong") {
    $("stat-rtt").textContent = `${Date.now() - Number(msg.t || state.lastPing)} ms`;
  } else if (msg.type === "chat") {
    addChat(msg.from || "对方", msg.text || "");
  }
}

async function startP2P(session) {
  closeP2P();
  const iceServers = session.ice_servers || state.iceServers || [];
  const pc = new RTCPeerConnection({ iceServers, iceTransportPolicy: "all" });
  state.pc = pc;
  const dc = pc.createDataChannel("session", { ordered: true });
  dc.binaryType = "arraybuffer";
  state.dc = dc;
  // Receive the screen as a WebRTC video track.
  pc.addTransceiver("video", { direction: "recvonly" });
  pc.addEventListener("track", (ev) => {
    const v = $("screen");
    if (v && ev.streams && ev.streams[0]) {
      v.srcObject = ev.streams[0];
      const p = v.play();
      if (p && p.catch) p.catch(() => {});
    }
  });
  dc.addEventListener("message", (ev) => onSessionMessage(ev.data));
  dc.addEventListener("open", () => {
    state.p2pReady = true;
    state.connMethod = null;
    setStatus("已连接", "busy");
    addChat("系统", "通道已接通，正在判定连接方式…");
    setTimeout(detectConnType, 600);
    setTimeout(detectConnType, 1800);
    setTimeout(detectConnType, 3500);
    startStats();
  });
  dc.addEventListener("close", () => {
    if (state.session) endSession("P2P 通道已关闭");
  });
  pc.addEventListener("iceconnectionstatechange", () => {
    if (pc.iceConnectionState === "failed") {
      sendSignal({ type: "signal", kind: "failed", message: "ice failed" });
      sendSignal({ type: "hangup", reason: "p2p_failed" });
      endSession("P2P 直连失败（没有中继回退）");
    }
  });
  const offer = await pc.createOffer();
  await pc.setLocalDescription(offer);
  await waitGathering(pc);
  sendSignal({
    type: "signal",
    kind: "offer",
    sdp: { type: pc.localDescription.type, sdp: maybeStripRelay(pc.localDescription.sdp) },
  });
}

async function handleSignal(msg) {
  if (!state.pc || msg.kind !== "answer" || !msg.sdp) return;
  await state.pc.setRemoteDescription(msg.sdp);
}

function closeP2P() {
  state.p2pReady = false;
  stopStats();
  const v = $("screen");
  if (v) { try { v.srcObject = null; } catch { /* ignore */ } }
  if (state.dc) {
    try { state.dc.close(); } catch { /* ignore */ }
    state.dc = null;
  }
  if (state.pc) {
    try { state.pc.close(); } catch { /* ignore */ }
    state.pc = null;
  }
}

function endSession(reason) {
  const wasSession = state.session;
  state.session = false;
  closeP2P();
  if (wasSession) {
    showView("home");
    setStatus("未连接", "offline");
    addChat("系统", reason);
  }
}

// ---- traffic / speed stats (from WebRTC getStats) ----
function startStats() {
  stopStats();
  state.lastBytes = 0;
  state.lastStatsAt = Date.now();
  state.sessionBytes = 0;
  state.statsTimer = setInterval(pollStats, 1000);
}
function stopStats() {
  if (state.statsTimer) { clearInterval(state.statsTimer); state.statsTimer = null; }
}
async function pollStats() {
  if (!state.pc) return;
  try {
    const stats = await state.pc.getStats();
    let bytes = 0;
    let fps = null;
    let w = null; let h = null;
    stats.forEach((r) => {
      if ((r.type === "inbound-rtp" && r.kind === "video") || r.type === "data-channel") {
        if (typeof r.bytesReceived === "number") bytes += r.bytesReceived;
      }
      if (r.type === "inbound-rtp" && r.kind === "video") {
        if (typeof r.framesPerSecond === "number") fps = r.framesPerSecond;
        if (typeof r.frameWidth === "number") { w = r.frameWidth; h = r.frameHeight; }
      }
    });
    const now = Date.now();
    if (state.lastBytes && bytes >= state.lastBytes) {
      const dt = (now - state.lastStatsAt) / 1000;
      const delta = bytes - state.lastBytes;
      if (dt > 0) $("stat-speed").textContent = fmtSpeed(delta / dt);
      // accumulate historical total
      const prev = Number(localStorage.getItem(TRAFFIC_KEY) || 0);
      localStorage.setItem(TRAFFIC_KEY, String(prev + delta));
    }
    state.sessionBytes = bytes;
    state.lastBytes = bytes;
    state.lastStatsAt = now;
    $("stat-session").textContent = `本次 ${fmtBytes(state.sessionBytes)}`;
    $("stat-total").textContent = `历史 ${fmtBytes(Number(localStorage.getItem(TRAFFIC_KEY) || 0))}`;
    if (fps != null) $("stat-fps").textContent = `${Math.round(fps)} FPS`;
    if (w) { state.screenW = w; state.screenH = h; $("stat-size").textContent = `${w}×${h}`; }
  } catch { /* ignore */ }
}

function canvasPoint(ev) {
  const el = $("screen");
  const rect = el.getBoundingClientRect();
  const iw = el.videoWidth || state.screenW;
  const ih = el.videoHeight || state.screenH;
  const x = ((ev.clientX - rect.left) / rect.width) * iw;
  const y = ((ev.clientY - rect.top) / rect.height) * ih;
  return { x: Math.round(x), y: Math.round(y) };
}

function bindInput() {
  const canvas = $("screen");
  const sendInput = (payload) => {
    if (!state.p2pReady) return;
    sendSession({ type: "input", ...payload });
  };
  canvas.addEventListener("mousemove", (ev) => {
    const p = canvasPoint(ev);
    sendInput({ event: "move", x: p.x, y: p.y });
  });
  canvas.addEventListener("mousedown", (ev) => {
    ev.preventDefault();
    canvas.focus();
    const p = canvasPoint(ev);
    const button = ev.button === 2 ? "right" : ev.button === 1 ? "middle" : "left";
    sendInput({ event: "down", x: p.x, y: p.y, button });
  });
  canvas.addEventListener("mouseup", (ev) => {
    const p = canvasPoint(ev);
    const button = ev.button === 2 ? "right" : ev.button === 1 ? "middle" : "left";
    sendInput({ event: "up", x: p.x, y: p.y, button });
  });
  canvas.addEventListener("wheel", (ev) => {
    ev.preventDefault();
    const p = canvasPoint(ev);
    sendInput({ event: "scroll", x: p.x, y: p.y, button: ev.deltaY < 0 ? "up" : "down" });
  }, { passive: false });
  canvas.addEventListener("contextmenu", (ev) => ev.preventDefault());
  canvas.addEventListener("keydown", (ev) => {
    ev.preventDefault();
    sendInput({ event: "keydown", key: ev.key });
  });
  canvas.addEventListener("keyup", (ev) => {
    ev.preventDefault();
    sendInput({ event: "keyup", key: ev.key });
  });
}

async function connect() {
  $("connect-error").hidden = true;
  const deviceId = $("remote-id").value;
  const password = $("remote-pass").value;
  if (!deviceId || !password) {
    $("connect-error").hidden = false;
    $("connect-error").textContent = "请填写识别码和密码";
    return;
  }
  setStatus("正在连接…", "busy");
  try {
    await ensureSocket();
    sendSignal({
      type: "connect",
      device_id: deviceId,
      password,
      name: $("viewer-name").value || "viewer",
    });
  } catch (err) {
    $("connect-error").hidden = false;
    $("connect-error").textContent = err.message;
    setStatus("未连接", "offline");
  }
}

function hangup() {
  sendSignal({ type: "hangup", reason: "viewer_hangup" });
  endSession("已主动断开");
}

function addFileLine(text) {
  const li = document.createElement("li");
  li.textContent = text;
  $("file-list").prepend(li);
}

async function sendFiles(fileList) {
  if (!state.p2pReady) {
    addFileLine("请先建立 P2P 连接");
    return;
  }
  for (const file of fileList) {
    const id = state.transferSeq++;
    sendSession({ type: "file_offer", id, name: file.name, size: file.size });
    const buf = await file.arrayBuffer();
    const bytes = new Uint8Array(buf);
    const chunk = 16 * 1024;
    for (let offset = 0; offset < bytes.length; offset += chunk) {
      const part = bytes.subarray(offset, offset + chunk);
      const header = new ArrayBuffer(13);
      const view = new DataView(header);
      view.setUint8(0, 2);
      view.setUint32(1, id);
      const hi = Math.floor(offset / 2 ** 32);
      const lo = offset >>> 0;
      view.setUint32(5, hi);
      view.setUint32(9, lo);
      const out = new Uint8Array(13 + part.length);
      out.set(new Uint8Array(header), 0);
      out.set(part, 13);
      state.dc.send(out);
    }
    sendSession({ type: "file_done", id, name: file.name });
    addFileLine(`已发送 ${file.name} (${file.size} 字节)`);
  }
}

function bindUi() {
  document.querySelectorAll(".nav-item").forEach((btn) => {
    btn.addEventListener("click", () => {
      const target = btn.dataset.view;
      if (target === "home" && state.session) {
        showView("session");
        return;
      }
      showView(target);
    });
  });
  $("remote-id").addEventListener("input", (ev) => {
    ev.target.value = formatId(ev.target.value);
  });
  $("btn-connect").addEventListener("click", connect);
  $("btn-hangup").addEventListener("click", hangup);
  $("copy-local").addEventListener("click", async () => {
    const text = `${$("local-id").textContent} / ${$("local-pass").textContent}`;
    await navigator.clipboard.writeText(text);
    $("copy-local").textContent = "已复制";
    setTimeout(() => { $("copy-local").textContent = "复制连接信息"; }, 1200);
  });
  $("chat-form").addEventListener("submit", (ev) => {
    ev.preventDefault();
    const text = $("chat-text").value.trim();
    if (!text) return;
    sendSession({ type: "chat", text, from: $("viewer-name").value || "viewer" });
    addChat("我", text);
    $("chat-text").value = "";
  });
  $("dropzone").addEventListener("click", () => $("file-input").click());
  $("file-input").addEventListener("change", (ev) => sendFiles(ev.target.files));
  $("dropzone").addEventListener("dragover", (ev) => ev.preventDefault());
  $("dropzone").addEventListener("drop", (ev) => {
    ev.preventDefault();
    sendFiles(ev.dataTransfer.files);
  });
  bindInput();
  setInterval(() => {
    if (state.p2pReady) {
      state.lastPing = Date.now();
      sendSession({ type: "ping", t: state.lastPing });
    }
  }, 2000);
}

loadConfig().catch(() => {});
bindUi();
