const A = window.dustxAgent;
const params = new URLSearchParams(location.search);
const state = {
  signalHttp: "",
  deviceId: String(params.get("id") || "").replace(/\D/g, ""),
  password: String(params.get("password") || ""),
  fromId: String(params.get("from") || "").replace(/\D/g, ""),
  token: String(params.get("token") || ""),
  local: params.get("local") === "1",
  cwd: "",
};

const logEl = document.getElementById("log");
const cwdEl = document.getElementById("cwd");
const form = document.getElementById("form");
const cmdEl = document.getElementById("cmd");

function add(text, cls) {
  const line = document.createElement("div");
  if (cls) line.className = cls;
  line.textContent = text;
  logEl.appendChild(line);
  logEl.scrollTop = logEl.scrollHeight;
}

async function run(body) {
  if (state.local || !state.deviceId) return A.localRun(body);
  return A.remoteRun(state.signalHttp, state.deviceId, state.password, body, {
    from_id: state.fromId,
    token: state.token,
  });
}

form.addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const command = cmdEl.value.trim();
  if (!command) return;
  cmdEl.value = "";
  if (command === "clear") {
    logEl.innerHTML = "";
    return;
  }
  if (command === "cd" || command.startsWith("cd ")) {
    const next = command.slice(2).trim() || "";
    const listed = await run({ op: "list", path: next || state.cwd, full: true });
    if (!listed.ok) {
      add(listed.error || "无法进入目录", "err");
      return;
    }
    state.cwd = listed.path || next;
    cwdEl.textContent = state.cwd || "~";
    add(`cd ${state.cwd}`, "meta");
    return;
  }
  add(`$ ${command}`, "cmd");
  const data = await run({ op: "exec", command, cwd: state.cwd, full: true });
  if (data.cwd) {
    state.cwd = data.cwd;
    cwdEl.textContent = state.cwd;
  }
  if (data.stdout) add(data.stdout.replace(/\n$/, ""));
  if (data.stderr) add(data.stderr.replace(/\n$/, ""), "err");
  if (!data.ok) add(data.error || "执行失败", "err");
  else if (data.exit) add(`exit ${data.exit}`, "meta");
});

async function boot() {
  try {
    const app = await (await fetch("/api/app")).json();
    state.signalHttp = app.signal_http || "";
  } catch { /* ignore */ }
  if (!state.signalHttp) state.signalHttp = "https://loessx.com";
  if (!state.fromId && !state.local) {
    try {
      const mesh = await (await fetch("/api/mesh")).json();
      state.fromId = String(mesh.device_id || "").replace(/\D/g, "");
    } catch { /* ignore */ }
  }
  const listed = await run({ op: "list", path: "", full: true });
  if (listed.ok) {
    state.cwd = listed.path || "";
    cwdEl.textContent = state.cwd || "~";
  } else {
    const err = listed.error || "";
    add(
      err === "wrong password"
        ? "密码不对。请用对方当前的远程码密码再连一次。"
        : (err || "对方不在线，或信令还没更新到支持终端的版本。"),
      "err"
    );
  }
  cmdEl.focus();
}

boot();
