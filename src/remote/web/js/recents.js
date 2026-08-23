const KEY = "dustx.recents";
const KINDS = ["remote", "mesh", "cam"];

function emptyDoc() {
  return { remote: [], mesh: [], cam: [] };
}

export function digits(id) {
  return String(id || "").replace(/\D/g, "");
}

function nowSec() {
  return Math.floor(Date.now() / 1000);
}

function normalizeItem(it) {
  if (!it || typeof it !== "object") return null;
  const item = {
    kind: String(it.kind || ""),
    id: digits(it.id),
    password: String(it.password || ""),
    pair_token: String(it.pair_token || ""),
    name: String(it.name || ""),
    transport: String(it.transport || ""),
    local_port: Number(it.local_port || 0) || 0,
    incoming: !!it.incoming,
    want: !!it.want,
    last_at: Number(it.last_at || 0) || 0,
  };
  if (Object.prototype.hasOwnProperty.call(it, "started_at")) {
    item.started_at = Number(it.started_at || 0) || 0;
  }
  if (Object.prototype.hasOwnProperty.call(it, "ended_at")) {
    item.ended_at = Number(it.ended_at || 0) || 0;
  }
  return item;
}

function unifyPasswords(doc) {
  const best = {};
  ["remote", "mesh"].forEach((kind) => {
    (doc[kind] || []).forEach((it) => {
      if (!it.id || !it.password) return;
      const prev = best[it.id];
      if (!prev || (it.last_at || 0) >= (prev.last_at || 0)) best[it.id] = it;
    });
  });
  ["remote", "mesh"].forEach((kind) => {
    (doc[kind] || []).forEach((it) => {
      if (best[it.id] && it.password !== best[it.id].password) it.password = best[it.id].password;
    });
  });
  return doc;
}

function sharePassword(doc, id, password) {
  const pid = digits(id);
  const pw = String(password || "");
  if (!pid || !pw) return doc;
  ["remote", "mesh"].forEach((kind) => {
    const list = doc[kind] || [];
    const idx = list.findIndex((x) => x.id === pid);
    if (idx >= 0) {
      list[idx].password = pw;
      return;
    }
    list.unshift({
      kind, id: pid, password: pw, name: "", transport: "",
      local_port: 0, incoming: false, want: false, last_at: nowSec(),
    });
    doc[kind] = list.slice(0, 20);
  });
  return doc;
}

function normalize(raw) {
  const doc = emptyDoc();
  if (!raw || typeof raw !== "object") return doc;
  KINDS.forEach((kind) => {
    const arr = Array.isArray(raw[kind]) ? raw[kind] : [];
    doc[kind] = arr.map((it) => {
      const item = normalizeItem(it);
      if (!item) return null;
      item.kind = kind;
      if (kind === "cam" && !item.id) item.id = item.transport || "wifi";
      return item;
    }).filter((it) => it && (kind === "cam" || it.id));
  });
  return unifyPasswords(doc);
}

function cache(doc) {
  try { localStorage.setItem(KEY, JSON.stringify(doc)); } catch { /* ignore */ }
  try {
    if (window.parent !== window) {
      window.parent.postMessage({ source: "dustx", recentsChanged: true }, "*");
    }
  } catch { /* ignore */ }
  return doc;
}

export async function loadRecents() {
  try {
    const res = await fetch("/api/recents");
    if (res.ok) return cache(normalize(await res.json()));
  } catch { /* official site */ }
  try { return normalize(JSON.parse(localStorage.getItem(KEY) || "{}")); }
  catch { return emptyDoc(); }
}

function upsertLocal(kind, item) {
  const doc = normalize(JSON.parse(localStorage.getItem(KEY) || "{}"));
  const next = normalizeItem(Object.assign({ kind }, item));
  if (!next) return doc;
  next.kind = kind;
  if (!next.last_at) next.last_at = nowSec();
  const key = kind === "cam" ? (next.transport || next.id || "wifi") : next.id;
  if (kind !== "cam" && !key) return doc;
  const list = doc[kind] || [];
  const idx = list.findIndex((x) => (kind === "cam" ? (x.transport || x.id) === key : x.id === key));
  const prev = idx >= 0 ? list[idx] : {};
  const merged = Object.assign({}, prev, next);
  if (!merged.password && prev.password) merged.password = prev.password;
  if (idx >= 0) list.splice(idx, 1);
  list.unshift(merged);
  doc[kind] = list.slice(0, 20);
  if (kind !== "cam" && merged.password) sharePassword(doc, merged.id, merged.password);
  return cache(doc);
}

export async function upsertRecent(kind, item) {
  const body = Object.assign({ kind, last_at: nowSec() }, item || {});
  if (kind !== "cam") body.id = digits(body.id);
  try {
    const res = await fetch("/api/recents", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
    });
    if (res.ok) return loadRecents();
  } catch { /* fallback */ }
  return upsertLocal(kind, body);
}

export async function forgetRecent(kind, id) {
  const key = kind === "cam" ? String(id || "") : digits(id);
  try {
    const res = await fetch("/api/recents/remove", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ kind, id: key, transport: kind === "cam" ? key : "" }),
    });
    if (res.ok) return loadRecents();
  } catch { /* fallback */ }
  const doc = normalize(JSON.parse(localStorage.getItem(KEY) || "{}"));
  doc[kind] = (doc[kind] || []).filter((x) => (kind === "cam" ? (x.transport || x.id) !== key : x.id !== key));
  return cache(doc);
}

export async function setRecentWant(kind, id, want) {
  return upsertRecent(kind, { id, want: !!want });
}

export function formatRecentWhen(ts) {
  const n = Number(ts || 0);
  if (!n) return "";
  const ms = n > 1e12 ? n : n * 1000;
  const d = new Date(ms);
  if (Number.isNaN(d.getTime())) return "";
  const p = (v) => String(v).padStart(2, "0");
  return `${d.getMonth() + 1}/${d.getDate()} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

export function formatRecentDuration(ms) {
  const s = Math.max(0, Math.floor(Number(ms || 0) / 1000));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h) return `${h}小时${m}分${sec}秒`;
  if (m) return `${m}分${sec}秒`;
  return `${sec}秒`;
}

export function formatRecentRange(started, ended) {
  const a = formatRecentWhen(started);
  const b = formatRecentWhen(ended);
  if (a && b) return `开始 ${a} · 结束 ${b}`;
  if (a) return `开始 ${a}`;
  if (b) return `结束 ${b}`;
  return "";
}

export function formatRecentLive(started) {
  const n = Number(started || 0);
  if (!n) return "";
  const ms = n > 1e12 ? n : n * 1000;
  return `开始 ${formatRecentWhen(n)} · 已持续 ${formatRecentDuration(Date.now() - ms)}`;
}

export function formatRecentRecord(it) {
  if (!it) return "";
  if (it.started_at && !it.ended_at) return formatRecentLive(it.started_at);
  const span = formatRecentRange(it.started_at, it.ended_at);
  if (span) return span;
  const last = formatRecentWhen(it.last_at);
  return last ? `上次 ${last}` : "";
}
