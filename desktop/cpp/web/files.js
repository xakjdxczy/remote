const A = window.dustxAgent;
const params = new URLSearchParams(location.search);
const state = {
  signalHttp: "",
  deviceId: String(params.get("id") || "").replace(/\D/g, ""),
  password: String(params.get("password") || ""),
  fromId: String(params.get("from") || "").replace(/\D/g, ""),
  token: String(params.get("token") || ""),
  localOnly: params.get("local") === "1",
  local: { path: "", home: "", entries: [], sel: null, volumes: false },
  remote: { path: "", home: "", entries: [], sel: null, volumes: false },
};

function $(sel) { return document.querySelector(sel); }

function normPath(p) {
  return String(p || "").replace(/[\\/]+$/, "").toLowerCase();
}

function looksLikeHome(p) {
  const s = String(p || "").replace(/[\\/]+$/, "");
  if (!s) return true;
  if (/^\/Users\/[^/]+$/.test(s) || /^\/home\/[^/]+$/.test(s)) return true;
  if (/^[A-Za-z]:\\Users\\[^\\]+$/.test(s)) return true;
  return false;
}

function atHome(pane) {
  if (!pane || pane.volumes) return false;
  if (pane.home && normPath(pane.path) === normPath(pane.home)) return true;
  return looksLikeHome(pane.path);
}

function fmtSize(n) {
  n = Number(n) || 0;
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MB`;
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GB`;
}

function setStatus(text, err) {
  const el = $("status");
  el.textContent = text;
  el.className = err ? "status err" : "status";
}

async function run(side, body) {
  if (side === "local" || state.localOnly) return A.localRun(body);
  if (!state.deviceId || (!state.password && !state.token)) return { ok: false, error: "没有对方识别码或密码" };
  return A.remoteRun(state.signalHttp, state.deviceId, state.password, body, {
    from_id: state.fromId,
    token: state.token,
  });
}

function render(side) {
  const pane = state[side];
  const list = $(`#list-${side}`);
  const crumb = $(`[data-crumb="${side}"]`);
  crumb.textContent = pane.volumes ? "磁盘" : (atHome(pane) ? "主目录" : (pane.path || "主目录"));
  const rows = pane.entries || [];
  if (!rows.length) {
    list.innerHTML = "<li><span></span><span class=\"name\">空文件夹</span></li>";
    return;
  }
  list.innerHTML = rows.map((it, i) => {
    const ico = it.dir ? "📁" : "📄";
    const size = it.dir ? "" : fmtSize(it.size);
    return `<li draggable="${it.dir ? "false" : "true"}" data-i="${i}">
      <span class="ico">${ico}</span>
      <span class="name">${it.name}</span>
      <span class="size">${size}</span>
    </li>`;
  }).join("");
}

async function loadDir(side, path, volumes) {
  const pane = state[side];
  pane.sel = null;
  const data = volumes
    ? await run(side, { op: "volumes" })
    : await run(side, { op: "list", path: path || "", home: !path, full: !!path });
  if (!data.ok) {
    pane.entries = [];
    pane.path = path || "";
    render(side);
    const list = $(`#list-${side}`);
    if (list) list.innerHTML = `<li><span></span><span class="name">${data.error || "无法列出目录"}</span></li>`;
    setStatus(data.error || "无法列出目录", true);
    return;
  }
  pane.volumes = !!volumes;
  pane.path = volumes ? "" : (data.path || path || "");
  if (!volumes && (data.home || !path || looksLikeHome(pane.path))) pane.home = pane.path || pane.home;
  pane.entries = (data.entries || []).map((e) => ({
    name: e.name,
    dir: !!e.dir,
    size: e.size || 0,
    path: e.path || A.joinPath(pane.path, e.name),
  }));
  render(side);
  setStatus(`${side === "local" ? "这台电脑" : "对方"} · ${pane.volumes ? "磁盘" : (atHome(pane) ? "主目录" : pane.path)}`);
}

async function enter(side, item) {
  const pane = state[side];
  if (pane.volumes) {
    await loadDir(side, item.path || item.name, false);
    return;
  }
  if (!item.dir) return;
  await loadDir(side, A.joinPath(pane.path, item.name), false);
}

function chunkBytes(chunk) {
  if (chunk.content_b64) return A.b64ToBytes(chunk.content_b64);
  if (chunk.content != null) {
    const enc = new TextEncoder().encode(String(chunk.content));
    return enc;
  }
  return new Uint8Array();
}

async function copyFile(fromSide, toSide, item) {
  if (!item || item.dir) {
    setStatus("请拖文件，文件夹请先进入后再传里面的文件", true);
    return;
  }
  const fromPath = item.path || A.joinPath(state[fromSide].path, item.name);
  const toPath = A.joinPath(state[toSide].path, item.name);
  setStatus(`正在传输 ${item.name} …`);
  let offset = 0;
  let size = Number(item.size) || 0;
  let first = true;
  while (true) {
    const chunk = await run(fromSide, {
      op: "read",
      path: fromPath,
      offset,
      length: A.CHUNK,
      full: true,
    });
    if (!chunk.ok) {
      setStatus(chunk.error || "读取失败", true);
      return;
    }
    if (first && !chunk.content_b64 && chunk.content != null) {
      const wrote = await run(toSide, { op: "write", path: toPath, content: chunk.content, full: true });
      if (!wrote.ok) {
        setStatus(wrote.error || "写入失败", true);
        return;
      }
      setStatus(`已传到 ${toPath}`);
      await loadDir(toSide, state[toSide].path, false);
      return;
    }
    first = false;
    if (chunk.size) size = Number(chunk.size);
    const raw = chunkBytes(chunk);
    if (!raw.length && offset >= size) break;
    const wrote = await run(toSide, {
      op: "write",
      path: toPath,
      content_b64: A.bytesToB64(raw),
      offset,
      full: true,
    });
    if (!wrote.ok) {
      setStatus(wrote.error || "写入失败", true);
      return;
    }
    offset += raw.length;
    setStatus(`正在传输 ${item.name} · ${fmtSize(offset)} / ${fmtSize(size)}`);
    if (offset >= size || raw.length === 0) break;
  }
  setStatus(`已传到 ${toPath}`);
  await loadDir(toSide, state[toSide].path, false);
}

async function uploadOsFiles(side, fileList) {
  const files = [...fileList];
  for (const file of files) {
    const dest = A.joinPath(state[side].path, file.name);
    setStatus(`正在上传 ${file.name} …`);
    const buf = new Uint8Array(await file.arrayBuffer());
    for (let offset = 0; offset < buf.length; offset += A.CHUNK) {
      const part = buf.subarray(offset, offset + A.CHUNK);
      const wrote = await run(side, {
        op: "write",
        path: dest,
        content_b64: A.bytesToB64(part),
        offset,
        full: true,
      });
      if (!wrote.ok) {
        setStatus(wrote.error || "上传失败", true);
        return;
      }
      setStatus(`正在上传 ${file.name} · ${fmtSize(offset + part.length)} / ${fmtSize(buf.length)}`);
    }
  }
  await loadDir(side, state[side].path, false);
  setStatus("上传完成");
}

function bindList(side) {
  const list = $(`#list-${side}`);
  list.addEventListener("click", (ev) => {
    const li = ev.target.closest("li[data-i]");
    if (!li) return;
    list.querySelectorAll("li").forEach((el) => el.classList.toggle("is-on", el === li));
    state[side].sel = state[side].entries[Number(li.dataset.i)];
  });
  list.addEventListener("dblclick", (ev) => {
    const li = ev.target.closest("li[data-i]");
    if (!li) return;
    const item = state[side].entries[Number(li.dataset.i)];
    enter(side, item);
  });
  list.addEventListener("dragstart", (ev) => {
    const li = ev.target.closest("li[data-i]");
    if (!li) return;
    const item = state[side].entries[Number(li.dataset.i)];
    ev.dataTransfer.setData("application/x-dustx-file", JSON.stringify({ side, item }));
    ev.dataTransfer.effectAllowed = "copy";
  });
  list.addEventListener("dragover", (ev) => {
    ev.preventDefault();
    list.classList.add("drop");
  });
  list.addEventListener("dragleave", () => list.classList.remove("drop"));
  list.addEventListener("drop", async (ev) => {
    ev.preventDefault();
    list.classList.remove("drop");
    if (ev.dataTransfer.files && ev.dataTransfer.files.length) {
      await uploadOsFiles(side, ev.dataTransfer.files);
      return;
    }
    const raw = ev.dataTransfer.getData("application/x-dustx-file");
    if (!raw) return;
    const payload = JSON.parse(raw);
    if (payload.side === side) return;
    await copyFile(payload.side, side, payload.item);
  });
}

document.addEventListener("click", async (ev) => {
  const up = ev.target.closest("[data-up]");
  const home = ev.target.closest("[data-home]");
  const vols = ev.target.closest("[data-vols]");
  const mkdir = ev.target.closest("[data-mkdir]");
  const rm = ev.target.closest("[data-rm]");
  const side = (up || home || vols || mkdir || rm)?.getAttribute("data-up")
    || (up || home || vols || mkdir || rm)?.getAttribute("data-home")
    || (up || home || vols || mkdir || rm)?.getAttribute("data-vols")
    || (up || home || vols || mkdir || rm)?.getAttribute("data-mkdir")
    || (up || home || vols || mkdir || rm)?.getAttribute("data-rm");
  if (!side) return;
  if (up) {
    if (state[side].volumes) return;
    if (atHome(state[side])) return;
    const parent = A.parentPath(state[side].path);
    if (!parent || (state[side].home && normPath(parent) === normPath(A.parentPath(state[side].home)))) {
      await loadDir(side, state[side].home || "", false);
      return;
    }
    if (!parent) await loadDir(side, "", true);
    else await loadDir(side, parent, false);
    return;
  }
  if (home) {
    await loadDir(side, "", false);
    return;
  }
  if (vols) {
    await loadDir(side, "", true);
    return;
  }
  if (mkdir) {
    const name = prompt("新文件夹名字");
    if (!name) return;
    const path = A.joinPath(state[side].path, name);
    const data = await run(side, { op: "mkdir", path, full: true });
    if (!data.ok) setStatus(data.error || "无法创建", true);
    else await loadDir(side, state[side].path, false);
    return;
  }
  if (rm) {
    const item = state[side].sel;
    if (!item) {
      setStatus("先点选一个文件或文件夹", true);
      return;
    }
    if (!confirm(`删除 ${item.name} ？`)) return;
    const path = item.path || A.joinPath(state[side].path, item.name);
    const data = await run(side, { op: "rm", path, full: true });
    if (!data.ok) setStatus(data.error || "无法删除", true);
    else await loadDir(side, state[side].path, false);
  }
});

async function boot() {
  try {
    const app = await (await fetch("/api/app")).json();
    state.signalHttp = app.signal_http || "";
  } catch { /* ignore */ }
  if (!state.signalHttp) state.signalHttp = "https://117.72.108.246";
  if (!state.fromId) {
    try {
      const mesh = await (await fetch("/api/mesh")).json();
      state.fromId = String(mesh.device_id || "").replace(/\D/g, "");
    } catch { /* ignore */ }
  }
  if (state.localOnly) {
    setStatus("文件传输是在本机和对方之间拷文件，请先选一台已连接的设备。", true);
    return;
  }
  if (!state.deviceId) {
    setStatus("没有对方识别码", true);
    return;
  }
  if (!state.password && !state.token) {
    setStatus("还没连接这台设备", true);
    return;
  }
  await loadDir("local", "", false);
  await loadDir("remote", "", false);
}

bindList("local");
bindList("remote");
boot().catch((e) => setStatus(String(e.message || e), true));
