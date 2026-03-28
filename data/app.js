const csvBox = document.getElementById("csvBox");
const msgBox = document.getElementById("msgBox");
const csvStatus = document.getElementById("csvStatus");
const msgStatus = document.getElementById("msgStatus");

function setStatus(el, text, isError = false) {
  el.textContent = text || "";
  el.classList.toggle("error", !!isError);
}

async function loadCsv() {
  try {
    const resp = await fetch("/api/csv");
    const text = await resp.text();
    if (!resp.ok) throw new Error(text || `HTTP ${resp.status}`);
    csvBox.value = text;
    setStatus(csvStatus, "Loaded /data.csv");
  } catch (err) {
    setStatus(csvStatus, `Load failed: ${err.message}`, true);
  }
}

async function saveCsv() {
  try {
    const body = new FormData();
    body.append("content", csvBox.value);
    const resp = await fetch("/api/csv", { method: "POST", body });
    const text = await resp.text();
    if (!resp.ok) throw new Error(text || `HTTP ${resp.status}`);
    setStatus(csvStatus, text || "Saved");
  } catch (err) {
    setStatus(csvStatus, `Save failed: ${err.message}`, true);
  }
}

async function sendMsg() {
  try {
    const body = new FormData();
    body.append("text", msgBox.value);
    const resp = await fetch("/api/send", { method: "POST", body });
    const text = await resp.text();
    if (!resp.ok) throw new Error(text || `HTTP ${resp.status}`);
    setStatus(msgStatus, text || "Queued");
    msgBox.value = "";
    await refreshStatus();
  } catch (err) {
    setStatus(msgStatus, `Send failed: ${err.message}`, true);
  }
}

async function refreshStatus() {
  try {
    const resp = await fetch("/api/status");
    const data = await resp.json();
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    setStatus(msgStatus, `Queue: ${data.queue} | CSV reload pending: ${data.csvReloadPending}`);
  } catch (err) {
    setStatus(msgStatus, `Status failed: ${err.message}`, true);
  }
}

document.getElementById("btnLoadCsv").addEventListener("click", loadCsv);
document.getElementById("btnSaveCsv").addEventListener("click", saveCsv);
document.getElementById("btnSendMsg").addEventListener("click", sendMsg);
document.getElementById("btnClearMsg").addEventListener("click", () => {
  msgBox.value = "";
  msgBox.focus();
});

setInterval(refreshStatus, 1000);
loadCsv();
refreshStatus();
