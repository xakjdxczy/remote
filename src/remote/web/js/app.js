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
  lastJbDelay: 0,
  lastJbCount: 0,
  lastDecTime: 0,
  lastDecFrames: 0,
  videoReceiver: null,
  lastQosAt: 0,
  dropUntilKey: false,
  keyChaseAttached: false,
  deviceId: "",
  password: "",
  role: null,
  pendingOffer: null,
  pendingCall: null,
  displayStream: null,
  pwTimer: null,
  registered: false,
};

// Chrome treats jitterBufferTarget=0 as "unset" on some versions and then
// keeps ~100ms. 16ms is a real low-latency target (half a 30fps frame).
const JB_TARGET_MS = 16;
const QOS_THROTTLE_MS = 1500;
const KEYFRAME_BUDGET_MS = 100;
const KEYFRAME_BPP = 0.75;
const SCENE_RESTORE_MS = 350;

function playbackRateForBuffer(jbMs) {
  if (jbMs >= 200) return 1.35;
  if (jbMs >= 120) return 1.18;
  if (jbMs >= 70) return 1.08;
  return 1;
}

function applyLowLatencyReceiver(receiver) {
  if (!receiver) return;
  try { receiver.playoutDelayHint = 0; } catch { /* not supported */ }
  try { receiver.jitterBufferTarget = JB_TARGET_MS; } catch { /* not supported */ }
}

function applyVideoBitrateFmtp(sdp, minKbps, startKbps, maxKbps) {
  const extra = `x-google-min-bitrate=${minKbps};x-google-start-bitrate=${startKbps};x-google-max-bitrate=${maxKbps}`;
  const nl = sdp.includes("\r\n") ? "\r\n" : "\n";
  let inVideo = false;
  return sdp.replace(/\r\n/g, "\n").split("\n").map((line) => {
    if (line.startsWith("m=")) inVideo = line.startsWith("m=video");
    if (inVideo && line.startsWith("a=fmtp:") && !line.includes("x-google-max-bitrate") && !line.includes("apt=")) {
      return `${line};${extra}`;
    }
    return line;
  }).join(nl);
}

async function preferH264(pc) {
  const caps = RTCRtpReceiver.getCapabilities?.("video");
  if (!caps || !caps.codecs) return;
  const h264 = caps.codecs.filter((c) => /h264/i.test(c.mimeType));
  if (!h264.length) return;
  const rest = caps.codecs.filter((c) => !/h264/i.test(c.mimeType));
  for (const t of pc.getTransceivers()) {
    if (t.receiver && (t.receiver.track == null || t.receiver.track.kind === "video")) {
      try { t.setCodecPreferences([...h264, ...rest]); } catch { /* not supported */ }
    }
  }
}

function keyframeBudgetBytes(bitrateBps, budgetMs = KEYFRAME_BUDGET_MS) {
  return Math.max(1024, Math.floor(bitrateBps * (budgetMs / 1000) / 8));
}

function evenDim(n) {
  const v = Math.max(2, Math.round(n));
  return v - (v % 2);
}

function sceneScaleSize(width, height, budgetBytes) {
  const raw = (width * height * KEYFRAME_BPP) / 8;
  if (raw <= budgetBytes) return { w: evenDim(width), h: evenDim(height), scale: 1 };
  const scale = Math.max(0.35, Math.min(1, Math.sqrt(budgetBytes / raw)));
  return { w: evenDim(width * scale), h: evenDim(height * scale), scale };
}

function currentCapBps() {
  return state.connMethod === "relay" ? 1_000_000 : 2_000_000;
}

function chaseLatestKeyframe() {
  state.dropUntilKey = true;
  const v = $("screen");
  if (v && Math.abs((v.playbackRate || 1) - 1.35) > 0.01) v.playbackRate = 1.35;
  applyLowLatencyReceiver(state.videoReceiver);
}

function attachKeyChase(receiver) {
  if (!receiver || state.keyChaseAttached || typeof receiver.createEncodedStreams !== "function") return;
  try {
    const { readable, writable } = receiver.createEncodedStreams();
    const xf = new TransformStream({
      transform(chunk, controller) {
        if (state.dropUntilKey) {
          if (chunk.type === "key") {
            state.dropUntilKey = false;
            controller.enqueue(chunk);
          }
          return;
        }
        controller.enqueue(chunk);
      },
    });
    readable.pipeThrough(xf).pipeTo(writable);
    state.keyChaseAttached = true;
  } catch {
    /* insertable streams not available */
  }
}

async function burstHostResolution() {
  const track = state.displayStream?.getVideoTracks?.()[0];
  if (!track) return;
  const settings = track.getSettings?.() || {};
  const fw = settings.width || 1280;
  const fh = settings.height || 720;
  const { w, h, scale } = sceneScaleSize(fw, fh, keyframeBudgetBytes(currentCapBps()));
  if (scale >= 0.98) return;
  try {
    await track.applyConstraints({ width: { ideal: w }, height: { ideal: h } });
  } catch { /* ignore */ }
  setTimeout(() => {
    track.applyConstraints({ width: { ideal: fw }, height: { ideal: fh } }).catch(() => {});
  }, SCENE_RESTORE_MS);
}

function applyCatchup(jbMs) {
  const v = $("screen");
  if (v) {
    const rate = playbackRateForBuffer(jbMs);
    if (Math.abs((v.playbackRate || 1) - rate) > 0.01) v.playbackRate = rate;
  }
  applyLowLatencyReceiver(state.videoReceiver);
  const now = Date.now();
  if (jbMs >= 120) chaseLatestKeyframe();
  if (jbMs >= 180 && now - state.lastQosAt >= QOS_THROTTLE_MS) {
    state.lastQosAt = now;
    sendSession({ type: "qos", buffer_ms: Math.round(jbMs), action: "keyframe" });
  } else if (jbMs >= 120 && now - state.lastQosAt >= QOS_THROTTLE_MS) {
    state.lastQosAt = now;
    sendSession({ type: "qos", buffer_ms: Math.round(jbMs), action: "backoff" });
  }
}

const TRAFFIC_KEY = "dustx_traffic_total_bytes";
const ID_KEY = "dustx_device_id";
const PW_KEY = "dustx_password";
const PW_REFRESH_KEY = "dustx_pw_refresh_sec";
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
      const proto = (local && (local.relayProtocol || local.protocol)) || (remote && remote.protocol) || "udp";
      applyConnMethod(method, proto);
      return;
    }
    applyConnMethod(method);
  } catch { /* ignore */ }
}

function applyConnMethod(method, proto) {
  const relay = method === "relay";
  const label = relay ? "TURN 中继" : "P2P 直连";
  const transport = proto ? String(proto).toUpperCase() : "";
  if (state.connMethod !== method) {
    state.connMethod = method;
    setStatus(label, "busy");
    addChat("系统", transport ? `${label} · ${transport}` : label);
    sendSession({ type: "conn_info", method, protocol: transport.toLowerCase() });
  }
  if ($("stat-path")) $("stat-path").textContent = label;
  if ($("stat-proto") && transport) $("stat-proto").textContent = transport;
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
    $("remote-id").value = cfg.demo_host.device_id_display;
    $("remote-pass").value = cfg.demo_host.password;
    $("local-hint").textContent = `演示主机 ${cfg.demo_host.device_id_display} 已在线。本机远程码见上方，连接演示主机需对方同意。`;
  }
  const savedRefresh = localStorage.getItem(PW_REFRESH_KEY);
  if (savedRefresh && $("pw-refresh")) $("pw-refresh").value = savedRefresh;
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
    state.registered = false;
    setStatus("信令断开", "offline");
    setTimeout(() => { goOnline().catch(() => {}); }, 1500);
  });
  return new Promise((resolve, reject) => {
    ws.addEventListener("open", () => resolve(ws), { once: true });
    ws.addEventListener("error", () => reject(new Error("无法连接信令服务器")), { once: true });
  });
}

async function goOnline() {
  await ensureSocket();
  if (state.registered) return;
  sendSignal({
    type: "register",
    hostname: $("viewer-name")?.value || (navigator.platform || "web"),
    os: navigator.userAgent || "browser",
    device_id: localStorage.getItem(ID_KEY) || "",
    temp_password: localStorage.getItem(PW_KEY) || "",
  });
}

function applyRegistered(msg) {
  state.registered = true;
  state.deviceId = msg.device_id;
  state.password = msg.temp_password;
  localStorage.setItem(ID_KEY, msg.device_id);
  localStorage.setItem(PW_KEY, msg.temp_password);
  if ($("local-id")) $("local-id").textContent = msg.device_id_display || formatId(msg.device_id);
  if ($("local-pass")) $("local-pass").textContent = msg.temp_password;
  if (!state.session) setStatus("在线", "online");
}

function showIncoming(msg) {
  state.pendingCall = msg;
  if ($("incoming-id")) $("incoming-id").textContent = msg.viewer_id_display || formatId(msg.viewer_id) || "未知";
  if ($("incoming-name")) $("incoming-name").textContent = msg.viewer_name ? `来自 ${msg.viewer_name}` : "有人请求远程控制本机";
  $("incoming-modal")?.classList.remove("hidden");
}

function hideIncoming() {
  $("incoming-modal")?.classList.add("hidden");
}

function answerCall(ok) {
  const call = state.pendingCall;
  hideIncoming();
  if (!call) return;
  sendSignal({ type: "auth_result", session_id: call.session_id, ok: !!ok });
  state.pendingCall = null;
  if (!ok) addChat("系统", "已拒绝来电");
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
  if (msg.type === "registered") {
    applyRegistered(msg);
  } else if (msg.type === "password") {
    applyRegistered({ device_id: state.deviceId, device_id_display: formatId(state.deviceId), temp_password: msg.temp_password });
    addChat("系统", "密码已刷新");
  } else if (msg.type === "auth_failed") {
    $("connect-error").hidden = false;
    $("connect-error").textContent = msg.message === "device offline"
      ? "设备不在线"
      : msg.message === "wrong password"
        ? "密码错误"
        : msg.message === "device busy"
          ? "设备正忙"
          : msg.message === "cannot connect to self"
            ? "不能连接自己的远程码"
            : msg.message || "连接失败";
    if (!state.session) setStatus("在线", "online");
  } else if (msg.type === "incoming_call") {
    showIncoming(msg);
  } else if (msg.type === "call_pending") {
    setStatus("等待对方同意…", "busy");
    addChat("系统", `已向 ${msg.host_id_display || msg.host_id} 发起连接，等待对方同意`);
  } else if (msg.type === "session_start") {
    const iAmHost = !!(state.deviceId && msg.host_id === state.deviceId);
    state.session = true;
    state.role = iAmHost ? "host" : "viewer";
    $("connect-error").hidden = true;
    hideIncoming();
    const peer = iAmHost
      ? (msg.viewer_id_display || formatId(msg.viewer_id) || msg.viewer_name)
      : (msg.host_id_display || formatId(msg.host_id) || msg.hostname);
    $("session-title").textContent = iAmHost ? `正在被控制 · ${peer}` : `远程桌面 · ${peer}`;
    if ($("stat-peer")) $("stat-peer").textContent = `对方 ${peer || "--"}`;
    $("hosting-banner")?.classList.toggle("hidden", !iAmHost);
    $("screen")?.classList.toggle("hidden", iAmHost);
    showView("session");
    setStatus("正在建立 P2P…", "busy");
    addChat("系统", iAmHost ? `已同意 ${peer}，正在共享本机画面` : `对方已同意，正在连接 ${peer}`);
    const start = iAmHost ? startHostP2P(msg) : startP2P(msg);
    start.catch((err) => {
      sendSignal({ type: "signal", kind: "failed", message: String(err) });
      sendSignal({ type: "hangup", reason: "p2p_failed" });
      endSession("P2P 直连失败");
    });
  } else if (msg.type === "session_end") {
    const reason = msg.reason === "rejected"
      ? "对方拒绝了连接"
      : msg.reason === "timeout"
        ? "对方未在时限内同意"
        : msg.reason === "replaced"
          ? "已有新的连接请求"
          : (msg.reason || "已断开");
    hideIncoming();
    endSession(reason, { notify: false });
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
    // Android hosts get a system navigation toolbar (back/home/recents/notifications)
    $("nav-bar")?.classList.toggle("hidden", msg.backend !== "android");
  } else if (msg.type === "ping") {
    sendSession({ type: "pong", t: msg.t }); // reply so the host can measure its RTT
  } else if (msg.type === "pong") {
    // network RTT is shown from getStats (see pollStats); pong is kept only so
    // the host can measure its own latency.
  } else if (msg.type === "chat") {
    addChat(msg.from || "对方", msg.text || "");
  } else if (msg.type === "scene_change") {
    chaseLatestKeyframe();
  } else if (msg.type === "qos" && state.role === "host") {
    if (msg.action === "keyframe" || (msg.buffer_ms || 0) >= 180) burstHostResolution();
  }
}

async function startHostP2P(session) {
  closeP2P();
  state.role = "host";
  let stream;
  try {
    stream = await navigator.mediaDevices.getDisplayMedia({
      video: { frameRate: 30, width: { max: 1280 }, height: { max: 720 } },
      audio: false,
    });
  } catch {
    sendSignal({ type: "hangup", reason: "screen_denied" });
    endSession("未授权共享屏幕");
    return;
  }
  state.displayStream = stream;
  stream.getVideoTracks()[0]?.addEventListener("ended", () => {
    sendSignal({ type: "hangup", reason: "screen_ended" });
    endSession("已停止共享屏幕");
  });
  const iceServers = session.ice_servers || state.iceServers || [];
  const pc = new RTCPeerConnection(rtcPeerConfig(iceServers));
  state.pc = pc;
  stream.getTracks().forEach((t) => pc.addTrack(t, stream));
  await preferH264(pc);
  pc.addEventListener("datachannel", (ev) => {
    const dc = ev.channel;
    dc.binaryType = "arraybuffer";
    state.dc = dc;
    dc.addEventListener("message", (e) => onSessionMessage(e.data));
    dc.addEventListener("open", () => {
      state.p2pReady = true;
      state.connMethod = null;
      setStatus("正在被控制", "busy");
      startStats();
      const settings = stream.getVideoTracks()[0]?.getSettings?.() || {};
      sendSession({
        type: "screen_info",
        width: settings.width || 1280,
        height: settings.height || 720,
        backend: "web",
      });
      setTimeout(detectConnType, 600);
      setTimeout(detectConnType, 1800);
    });
    dc.addEventListener("close", () => {
      if (state.session) endSession("P2P 通道已关闭");
    });
  });
  if (state.pendingOffer) {
    const offer = state.pendingOffer;
    state.pendingOffer = null;
    await answerOffer(offer);
  }
}

async function answerOffer(sdp) {
  if (!state.pc) return;
  await state.pc.setRemoteDescription(sdp);
  const answer = await state.pc.createAnswer();
  await state.pc.setLocalDescription(answer);
  await waitGathering(state.pc);
  sendSignal({
    type: "signal",
    kind: "answer",
    sdp: { type: state.pc.localDescription.type, sdp: maybeStripRelay(state.pc.localDescription.sdp) },
  });
}

async function startP2P(session) {
  closeP2P();
  state.role = "viewer";
  const iceServers = session.ice_servers || state.iceServers || [];
  const pc = new RTCPeerConnection(rtcPeerConfig(iceServers));
  state.pc = pc;
  const dc = pc.createDataChannel("session", { ordered: true });
  dc.binaryType = "arraybuffer";
  state.dc = dc;
  // Receive the screen as a WebRTC video track.
  pc.addTransceiver("video", { direction: "recvonly" });
  await preferH264(pc);
  pc.addEventListener("track", (ev) => {
    state.videoReceiver = ev.receiver;
    applyLowLatencyReceiver(ev.receiver);
    attachKeyChase(ev.receiver);
    const v = $("screen");
    if (v && ev.streams && ev.streams[0]) {
      v.srcObject = ev.streams[0];
      v.playsInline = true;
      v.muted = true;
      v.playbackRate = 1;
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
      endSession("P2P 直连失败（没有中继回退）");
    }
  });
  const offer = await pc.createOffer();
  // Conservative start (TURN is common). Host raises the cap on P2P via conn_info.
  const offerSdp = applyVideoBitrateFmtp(offer.sdp || "", 250, 500, 1200);
  try {
    await pc.setLocalDescription({ type: offer.type, sdp: offerSdp });
  } catch {
    await pc.setLocalDescription(offer);
  }
  await waitGathering(pc);
  sendSignal({
    type: "signal",
    kind: "offer",
    sdp: { type: pc.localDescription.type, sdp: maybeStripRelay(pc.localDescription.sdp) },
  });
}

async function handleSignal(msg) {
  if (msg.kind === "offer" && msg.sdp) {
    if (!state.pc) {
      state.pendingOffer = msg.sdp;
      return;
    }
    await answerOffer(msg.sdp);
    return;
  }
  if (!state.pc || msg.kind !== "answer" || !msg.sdp) return;
  await state.pc.setRemoteDescription(msg.sdp);
}

function rtcPeerConfig(iceServers) {
  const cfg = { iceServers, iceTransportPolicy: "all", bundlePolicy: "max-bundle" };
  try {
    const probe = new RTCPeerConnection({ encodedInsertableStreams: true });
    probe.close();
    cfg.encodedInsertableStreams = true;
  } catch { /* older browsers */ }
  return cfg;
}

function closeP2P() {
  state.p2pReady = false;
  state.videoReceiver = null;
  state.lastQosAt = 0;
  state.dropUntilKey = false;
  state.keyChaseAttached = false;
  state.pendingOffer = null;
  if (state.displayStream) {
    try { state.displayStream.getTracks().forEach((t) => t.stop()); } catch { /* ignore */ }
    state.displayStream = null;
  }
  const screen = $("screen");
  if (screen) screen.playbackRate = 1;
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

function endSession(reason, { notify = true } = {}) {
  const wasSession = state.session;
  if (wasSession && notify) {
    sendSignal({ type: "hangup", reason: "viewer_end" });
  }
  state.session = false;
  state.role = null;
  $("nav-bar")?.classList.add("hidden");
  $("hosting-banner")?.classList.add("hidden");
  $("screen")?.classList.remove("hidden");
  closeP2P();
  if (wasSession) {
    showView("home");
    setStatus(state.registered ? "在线" : "未连接", state.registered ? "online" : "offline");
    addChat("系统", reason);
  }
}

// ---- traffic / speed stats (from WebRTC getStats) ----
function startStats() {
  stopStats();
  state.lastBytes = 0;
  state.lastStatsAt = Date.now();
  state.sessionBytes = 0;
  state.statsTimer = setInterval(pollStats, 400);
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
    let jbDelay = null; let jbCount = null;
    let decTime = null; let decFrames = null;
    let rttMs = null;
    let selPairId = null;
    const codecs = {};
    let inCodec = null;
    let outCodec = null;
    stats.forEach((r) => {
      if (r.type === "transport" && r.selectedCandidatePairId) selPairId = r.selectedCandidatePairId;
      if (r.type === "codec" && r.mimeType) codecs[r.id] = String(r.mimeType).replace(/^video\//i, "").replace(/^audio\//i, "");
    });
    stats.forEach((r) => {
      if ((r.type === "inbound-rtp" && r.kind === "video") || r.type === "data-channel") {
        if (typeof r.bytesReceived === "number") bytes += r.bytesReceived;
      }
      if (r.type === "outbound-rtp" && r.kind === "video" && typeof r.bytesSent === "number") {
        bytes += r.bytesSent;
      }
      if (r.type === "inbound-rtp" && r.kind === "video") {
        if (typeof r.framesPerSecond === "number") fps = r.framesPerSecond;
        if (typeof r.frameWidth === "number") { w = r.frameWidth; h = r.frameHeight; }
        if (typeof r.jitterBufferDelay === "number") jbDelay = r.jitterBufferDelay;
        if (typeof r.jitterBufferEmittedCount === "number") jbCount = r.jitterBufferEmittedCount;
        if (typeof r.totalDecodeTime === "number") decTime = r.totalDecodeTime;
        if (typeof r.framesDecoded === "number") decFrames = r.framesDecoded;
        if (r.codecId && codecs[r.codecId]) inCodec = codecs[r.codecId];
      }
      if (r.type === "outbound-rtp" && r.kind === "video" && r.codecId && codecs[r.codecId]) {
        outCodec = codecs[r.codecId];
      }
      if (r.type === "candidate-pair" && typeof r.currentRoundTripTime === "number") {
        if ((selPairId && r.id === selPairId) || r.nominated || r.selected) rttMs = r.currentRoundTripTime * 1000;
      }
    });
    // per-interval jitter-buffer and decode latency (ms)
    let jbMs = null;
    if (jbDelay != null && jbCount != null && jbCount > state.lastJbCount) {
      jbMs = (jbDelay - state.lastJbDelay) / (jbCount - state.lastJbCount) * 1000;
      state.lastJbDelay = jbDelay; state.lastJbCount = jbCount;
    }
    let decMs = null;
    if (decTime != null && decFrames != null && decFrames > state.lastDecFrames) {
      decMs = (decTime - state.lastDecTime) / (decFrames - state.lastDecFrames) * 1000;
      state.lastDecTime = decTime; state.lastDecFrames = decFrames;
    }
    const parts = [];
    if (rttMs != null) parts.push(`网络${Math.round(rttMs)}`);
    if (jbMs != null) {
      parts.push(`缓冲${Math.round(jbMs)}`);
      applyCatchup(jbMs);
    }
    if (decMs != null) parts.push(`解码${Math.round(decMs)}`);
    if (parts.length && $("stat-rtt")) $("stat-rtt").textContent = parts.join("·") + " ms";
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
    if (fps != null && $("stat-fps")) $("stat-fps").textContent = `${Math.round(fps)} FPS`;
    if (w) { state.screenW = w; state.screenH = h; $("stat-size").textContent = `${w}×${h}`; }
    const codecBits = [];
    if (outCodec) codecBits.push(`编码 ${outCodec.toUpperCase()}`);
    if (inCodec) codecBits.push(`解码 ${inCodec.toUpperCase()}`);
    if (codecBits.length && $("stat-codec")) $("stat-codec").textContent = codecBits.join(" · ");
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

function updateCursorOverlay(ev, p) {
  const el = $("screen");
  const rect = el.getBoundingClientRect();
  const x = ev.clientX - rect.left;
  const y = ev.clientY - rect.top;
  const cur = $("remote-cursor");
  const lbl = $("cursor-coord");
  if (cur) { cur.style.left = `${x}px`; cur.style.top = `${y}px`; cur.classList.remove("hidden"); }
  if (lbl) { lbl.style.left = `${x}px`; lbl.style.top = `${y}px`; lbl.textContent = `${p.x}, ${p.y}`; lbl.classList.remove("hidden"); }
}
function hideCursorOverlay() {
  $("remote-cursor")?.classList.add("hidden");
  $("cursor-coord")?.classList.add("hidden");
}

function bindInput() {
  const canvas = $("screen");
  const sendInput = (payload) => {
    if (!state.p2pReady) return;
    sendSession({ type: "input", ...payload });
  };
  canvas.addEventListener("mousemove", (ev) => {
    const p = canvasPoint(ev);
    updateCursorOverlay(ev, p);
    sendInput({ event: "move", x: p.x, y: p.y });
  });
  canvas.addEventListener("mouseenter", (ev) => updateCursorOverlay(ev, canvasPoint(ev)));
  canvas.addEventListener("mouseleave", hideCursorOverlay);
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

function schedulePasswordRefresh() {
  if (state.pwTimer) {
    clearInterval(state.pwTimer);
    state.pwTimer = null;
  }
  const sec = Number(localStorage.getItem(PW_REFRESH_KEY) || 0);
  if (!sec) return;
  state.pwTimer = setInterval(() => {
    if (state.registered) sendSignal({ type: "refresh_password" });
  }, sec * 1000);
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
  const target = String(deviceId).replace(/\D/g, "");
  if (state.deviceId && target === state.deviceId) {
    $("connect-error").hidden = false;
    $("connect-error").textContent = "不能连接自己的远程码";
    return;
  }
  setStatus("正在连接…", "busy");
  try {
    await goOnline();
    sendSignal({
      type: "connect",
      device_id: deviceId,
      password,
      name: $("viewer-name").value || "web",
    });
  } catch (err) {
    $("connect-error").hidden = false;
    $("connect-error").textContent = err.message;
    setStatus("未连接", "offline");
  }
}

function hangup() {
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
  $("btn-accept")?.addEventListener("click", () => answerCall(true));
  $("btn-reject")?.addEventListener("click", () => answerCall(false));
  $("btn-refresh-pw")?.addEventListener("click", () => {
    if (state.registered) sendSignal({ type: "refresh_password" });
  });
  $("pw-refresh")?.addEventListener("change", (ev) => {
    localStorage.setItem(PW_REFRESH_KEY, ev.target.value);
    schedulePasswordRefresh();
  });
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
  document.querySelectorAll("#nav-bar button").forEach((b) => {
    b.addEventListener("click", () => {
      if (state.p2pReady) sendSession({ type: "nav", action: b.dataset.nav });
    });
  });
  bindInput();
  setInterval(() => {
    if (state.p2pReady) {
      state.lastPing = Date.now();
      sendSession({ type: "ping", t: state.lastPing });
    }
  }, 2000);
}

loadConfig()
  .then(() => goOnline())
  .then(() => schedulePasswordRefresh())
  .catch(() => {});
bindUi();
