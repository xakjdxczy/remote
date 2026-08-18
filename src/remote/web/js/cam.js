const cam = {
  ws: null,
  pc: null,
  token: "",
  ice: [],
  transport: "wifi",
};

function camEl(id) {
  return document.getElementById(id);
}

function setCamStatus(text, cls) {
  const pill = camEl("cam-status");
  if (!pill) return;
  pill.textContent = text;
  pill.className = `pill ${cls || ""}`;
}

function camWsUrl() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  return `${proto}://${location.host}/cam/ws`;
}

async function refreshCamInfo() {
  const res = await fetch("/api/cam");
  const data = await res.json();
  cam.token = data.token;
  cam.ice = data.ice_servers || [];
  camEl("cam-token").textContent = data.token;
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
  const ws = new WebSocket(camWsUrl());
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
      startCamOffer();
    }
    if (msg.type === "signal" && msg.kind === "answer") {
      const sdp = msg.sdp || {};
      cam.pc?.setRemoteDescription({ type: sdp.type || "answer", sdp: sdp.sdp || "" });
    }
    if (msg.type === "peer_left") {
      setCamStatus("手机已断开", "");
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
  if (cam.pc) {
    try { cam.pc.close(); } catch { /* ignore */ }
    cam.pc = null;
  }
  const video = camEl("cam-video");
  if (video) video.srcObject = null;
}

async function rotateCamToken() {
  await fetch("/api/cam/rotate", { method: "POST" });
  teardownCamPeer();
  await refreshCamInfo();
  connectCamDesktop();
}

async function startCamSink() {
  const status = camEl("cam-sink-status");
  status.textContent = "正在切换到系统虚拟设备…";
  try { cam.ws?.close(); } catch { /* ignore */ }
  teardownCamPeer();
  const res = await fetch("/api/cam/sink/start", { method: "POST" });
  const data = await res.json();
  status.textContent = data.message || "已开始输出";
  setCamStatus("虚拟设备输出中", "busy");
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

export function bindCamUi() {
  if (!camEl("view-cam")) return;
  camEl("cam-mode-wifi")?.addEventListener("click", () => {
    cam.transport = "wifi";
    refreshCamInfo();
  });
  camEl("cam-mode-usb")?.addEventListener("click", () => {
    cam.transport = "usb";
    refreshCamInfo();
  });
  camEl("cam-rotate")?.addEventListener("click", () => rotateCamToken());
  camEl("cam-wait")?.addEventListener("click", () => connectCamDesktop());
  camEl("cam-adb")?.addEventListener("click", () => prepareUsbAdb());
  camEl("cam-sink")?.addEventListener("click", () => startCamSink());
  refreshCamInfo().then(() => connectCamDesktop()).catch((err) => {
    setCamStatus(String(err), "");
  });
}
