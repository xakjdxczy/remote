(function (root) {
  const CHUNK = 384 * 1024;

  function bytesToB64(bytes) {
    let s = "";
    const step = 0x8000;
    for (let i = 0; i < bytes.length; i += step) {
      s += String.fromCharCode.apply(null, bytes.subarray(i, i + step));
    }
    return btoa(s);
  }

  function b64ToBytes(b64) {
    const bin = atob(b64 || "");
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  async function localRun(body) {
    const res = await fetch("/api/agent/run", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    return res.json();
  }

  async function remoteRun(signalHttp, deviceId, password, body, extra) {
    const more = extra && typeof extra === "object" ? extra : {};
    const res = await fetch(`${String(signalHttp || "").replace(/\/$/, "")}/api/agent`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(Object.assign({
        device_id: deviceId,
        password,
        from_id: more.from_id || "",
        token: more.token || "",
      }, body)),
    });
    let data = {};
    try { data = await res.json(); } catch { data = {}; }
    if (!res.ok) return { ok: false, error: data.error || res.statusText || "agent failed" };
    return data;
  }

  function joinPath(dir, name) {
    const n = String(name || "");
    if (!dir) return n;
    if (/^[A-Za-z]:\\?$/.test(dir) || /[\\/]$/.test(dir)) {
      const sep = dir.includes("\\") ? "\\" : "/";
      return dir.replace(/[\\/]+$/, "") + sep + n;
    }
    const sep = dir.includes("\\") ? "\\" : "/";
    return dir + sep + n;
  }

  function parentPath(dir) {
    const s = String(dir || "");
    if (!s || s === "/" || /^[A-Za-z]:\\?$/.test(s)) return "";
    const trimmed = s.replace(/[\\/]+$/, "");
    const idx = Math.max(trimmed.lastIndexOf("/"), trimmed.lastIndexOf("\\"));
    if (idx <= 0) return s.includes("\\") ? s.slice(0, 3) : "/";
    if (idx === 2 && trimmed[1] === ":") return trimmed.slice(0, 3);
    return trimmed.slice(0, idx) || "/";
  }

  root.dustxAgent = {
    CHUNK,
    bytesToB64,
    b64ToBytes,
    localRun,
    remoteRun,
    joinPath,
    parentPath,
  };
})(window);
