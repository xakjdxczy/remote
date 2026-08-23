const cam = {
  ws: null,
  media: null,
  pc: null,
  token: "",
  ice: [],
  transport: "wifi",
  pumping: false,
  startedAt: 0,
};

function camEl(id) {
  return document.getElementById(id);
}

function rememberCam(extra) {
  if (!window.dustxRecents) return;
  window.dustxRecents.upsert("cam", Object.assign({
    id: cam.transport || "wifi",
    transport: cam.transport || "wifi",
    want: true,
  }, extra || {})).catch(() => {});
}

function applyCamTransport(transport) {
  cam.transport = transport === "usb" ? "usb" : "wifi";
  rememberCam();
  refreshCamInfo();
}

function notifyCam() {
  if (typeof dustxNotifyShell !== "function") return;
  const pill = camEl("cam-status");
  dustxNotifyShell({
    channel: "cam",
    status: pill ? pill.textContent : "",
    cls: pill ? String(pill.className).replace("pill", "").trim() : "",
    id: cam.token || "",
    idDisplay: camEl("cam-token")?.textContent || cam.token || "",
  });
}

function setCamStatus(text, cls) {
  const pill = camEl("cam-status");
  if (!pill) return;
  pill.textContent = text;
  pill.className = `pill ${cls || ""}`;
  notifyCam();
}

function camWsUrl(path) {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  return `${proto}://${location.host}${path}`;
}

async function refreshCamInfo() {
  const res = await fetch("/api/cam");
  const data = await res.json();
  cam.token = data.token;
  cam.ice = data.ice_servers || [];
  camEl("cam-token").textContent = data.token;
  notifyCam();
  const ips = (data.ips || []).join("  ") || "（未发现局域网 IP，请用 USB / 127.0.0.1）";
  camEl("cam-ips").textContent = ips;
  camEl("cam-usb-cmd").textContent = data.usb?.adb_reverse || "";
  camEl("cam-usb-hint").textContent = data.usb?.hint || "";
  const pair = (cam.transport === "usb" ? data.usb?.pair_url : (data.pair_urls || [])[0]) || "";
  const pairEl = camEl("cam-pair-url");
  if (pairEl) pairEl.textContent = pair;
  const qr = camEl("cam-qr");
  if (qr && pair) {
    qr.hidden = false;
    qr.src = `https://api.qrserver.com/v1/create-qr-code/?size=180x180&data=${encodeURIComponent(pair)}`;
  } else if (qr) {
    qr.hidden = true;
  }
  renderCamTransport();
}

function renderCamTransport() {
  const wifi = cam.transport === "wifi";
  camEl("cam-wifi-help").hidden = !wifi;
  camEl("cam-usb-help").hidden = wifi;
}

async function connectCamDesktop() {
  if (cam.ws) {
    try { cam.ws.close(); } catch { /* ignore */ }
  }
  setCamStatus("等待手机…", "busy");
  const ws = new WebSocket(camWsUrl("/cam/ws"));
  cam.ws = ws;
  ws.onopen = () => {
    ws.send(JSON.stringify({ type: "hello", role: "desktop", token: cam.token }));
  };
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.type === "error") {
      setCamStatus(msg.message || "错误", "");
      return;
    }
    if (msg.type === "ready") {
      setCamStatus("手机已接入，正在拉流", "busy");
      rememberCam();
      startCamOffer();
    }
    if (msg.type === "signal" && msg.kind === "answer") {
      const sdp = msg.sdp || {};
      cam.pc?.setRemoteDescription({ type: sdp.type || "answer", sdp: sdp.sdp || "" });
    }
    if (msg.type === "peer_left") {
      setCamStatus("手机已断开", "");
      if (cam.startedAt) {
        rememberCam({
          started_at: Math.floor(cam.startedAt / 1000),
          ended_at: Math.floor(Date.now() / 1000),
        });
        cam.startedAt = 0;
      }
      teardownCamPeer();
    }
  };
  ws.onclose = () => {
    if (cam.ws === ws) setCamStatus("本机通道已关闭", "");
  };
}

async function startCamOffer() {
  teardownCamPeer();
  const pc = new RTCPeerConnection({ iceServers: cam.ice });
  cam.pc = pc;
  pc.addTransceiver("video", { direction: "recvonly" });
  pc.addTransceiver("audio", { direction: "recvonly" });
  pc.ontrack = (ev) => {
    const video = camEl("cam-video");
    if (video.srcObject !== ev.streams[0]) video.srcObject = ev.streams[0];
    video.play().catch(() => {});
    setCamStatus("已连接", "online");
    if (!cam.startedAt) cam.startedAt = Date.now();
    rememberCam({
      started_at: Math.floor(cam.startedAt / 1000),
      ended_at: 0,
    });
  };
  pc.oniceconnectionstatechange = () => {
    if (pc.iceConnectionState === "failed") setCamStatus("ICE 失败", "");
  };
  const offer = await pc.createOffer();
  await pc.setLocalDescription(offer);
  await new Promise((resolve) => {
    if (pc.iceGatheringState === "complete") {
      resolve();
      return;
    }
    const timer = setTimeout(resolve, 2500);
    pc.onicegatheringstatechange = () => {
      if (pc.iceGatheringState === "complete") {
        clearTimeout(timer);
        resolve();
      }
    };
  });
  cam.ws?.send(JSON.stringify({
    type: "signal",
    kind: "offer",
    sdp: { type: pc.localDescription.type, sdp: pc.localDescription.sdp },
  }));
}

function teardownCamPeer() {
  stopFramePump();
  if (cam.pc) {
    try { cam.pc.close(); } catch { /* ignore */ }
    cam.pc = null;
  }
  const video = camEl("cam-video");
  if (video) video.srcObject = null;
}

function packRgbFrame(rgb, w, h) {
  const out = new Uint8Array(5 + rgb.byteLength);
  out[0] = 1;
  out[1] = w & 255;
  out[2] = (w >> 8) & 255;
  out[3] = h & 255;
  out[4] = (h >> 8) & 255;
  out.set(rgb, 5);
  return out;
}

function packPcm(floats) {
  const out = new Uint8Array(5 + floats.byteLength);
  out[0] = 2;
  const n = floats.length;
  out[1] = n & 255;
  out[2] = (n >> 8) & 255;
  out[3] = (n >> 16) & 255;
  out[4] = (n >> 24) & 255;
  out.set(new Uint8Array(floats.buffer, floats.byteOffset, floats.byteLength), 5);
  return out;
}

function startFramePump() {
  if (cam.pumping) return;
  const video = camEl("cam-video");
  if (!video?.srcObject) return;
  cam.pumping = true;
  const ws = new WebSocket(camWsUrl("/cam/media"));
  cam.media = ws;
  ws.binaryType = "arraybuffer";
  const canvas = document.createElement("canvas");
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  let last = 0;
  const draw = (ts) => {
    if (!cam.pumping) return;
    requestAnimationFrame(draw);
    if (ts - last < 66) return;
    last = ts;
    if (ws.readyState !== 1 || video.videoWidth < 2) return;
    const w = Math.min(1280, video.videoWidth);
    const h = Math.round(video.videoHeight * (w / video.videoWidth));
    canvas.width = w;
    canvas.height = h;
    ctx.drawImage(video, 0, 0, w, h);
    const img = ctx.getImageData(0, 0, w, h).data;
    const rgb = new Uint8Array(w * h * 3);
    for (let i = 0, j = 0; i < img.length; i += 4, j += 3) {
      rgb[j] = img[i];
      rgb[j + 1] = img[i + 1];
      rgb[j + 2] = img[i + 2];
    }
    ws.send(packRgbFrame(rgb, w, h));
  };
  ws.onopen = () => requestAnimationFrame(draw);
  const stream = video.srcObject;
  const audio = stream.getAudioTracks?.()[0];
  if (audio && window.AudioContext) {
    const ac = new AudioContext();
    const src = ac.createMediaStreamSource(new MediaStream([audio]));
    const node = ac.createScriptProcessor(2048, 1, 1);
    node.onaudioprocess = (ev) => {
      if (ws.readyState !== 1) return;
      const data = ev.inputBuffer.getChannelData(0);
      ws.send(packPcm(data));
    };
    src.connect(node);
    node.connect(ac.destination);
    cam._audio = { ac, node, src };
  }
}

function stopFramePump() {
  cam.pumping = false;
  if (cam.media) {
    try { cam.media.close(); } catch { /* ignore */ }
    cam.media = null;
  }
  if (cam._audio) {
    try { cam._audio.node.disconnect(); } catch { /* ignore */ }
    try { cam._audio.ac.close(); } catch { /* ignore */ }
    cam._audio = null;
  }
}

async function rotateCamToken() {
  await fetch("/api/cam/rotate", { method: "POST" });
  teardownCamPeer();
  await refreshCamInfo();
  connectCamDesktop();
}

async function startCamSink() {
  const status = camEl("cam-sink-status");
  status.textContent = "正在打开系统虚拟设备…";
  const res = await fetch("/api/cam/sink/start", { method: "POST" });
  const data = await res.json();
  status.textContent = data.message || "已开始输出";
  startFramePump();
  setCamStatus(cam.pc ? "虚拟设备输出中" : "等待手机画面", "busy");
}

async function prepareUsbAdb() {
  const status = camEl("cam-adb-status");
  status.textContent = "正在执行 adb reverse…";
  try {
    const res = await fetch("/api/cam/adb", { method: "POST" });
    const data = await res.json();
    status.textContent = data.message || (data.ok ? "已转发，手机填 127.0.0.1" : "adb 失败");
  } catch (err) {
    status.textContent = `无法调用 adb：${err}`;
  }
}

camEl("cam-mode-wifi")?.addEventListener("click", () => applyCamTransport("wifi"));
camEl("cam-mode-usb")?.addEventListener("click", () => applyCamTransport("usb"));
camEl("cam-rotate")?.addEventListener("click", () => rotateCamToken());
camEl("cam-wait")?.addEventListener("click", () => connectCamDesktop());
camEl("cam-adb")?.addEventListener("click", () => prepareUsbAdb());
camEl("cam-sink")?.addEventListener("click", () => startCamSink());
window.addEventListener("message", (ev) => {
  const msg = ev.data;
  if (!msg || msg.source !== "dustx-shell" || msg.action !== "resume" || msg.kind !== "cam") return;
  applyCamTransport(msg.item?.transport || cam.transport);
  connectCamDesktop();
});
(async () => {
  try {
    const doc = await window.dustxRecents?.load();
    const last = (doc?.cam || [])[0];
    if (last?.transport) cam.transport = last.transport === "usb" ? "usb" : "wifi";
  } catch { /* ignore */ }
  refreshCamInfo().then(() => connectCamDesktop()).catch((err) => {
    setCamStatus(String(err), "");
  });
})();
