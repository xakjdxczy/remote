async function refreshLogs() {
  const box = document.getElementById("dustx-log");
  const path = document.getElementById("dustx-log-path");
  if (!box) return;
  try {
    const res = await fetch("/api/logs");
    const data = await res.json();
    if (path) path.textContent = data.path ? `日志文件：${data.path}` : "";
    box.textContent = (data.lines || []).join("\n") || "暂无日志";
    box.scrollTop = box.scrollHeight;
  } catch {
    box.textContent = "暂时读不到日志";
  }
}

window.refreshLogs = refreshLogs;
setInterval(refreshLogs, 1500);
refreshLogs();
