const csvBox = document.getElementById("csvBox");
const msgBox = document.getElementById("msgBox");
const csvStatus = document.getElementById("csvStatus");
const msgStatus = document.getElementById("msgStatus");
const volumeSlider = document.getElementById("volumeSlider");
const volumeValue = document.getElementById("volumeValue");
const volumeStatus = document.getElementById("volumeStatus");

let volumePushTimer = null;

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
    if (typeof data.volume === "number" && document.activeElement !== volumeSlider) {
      setVolumeUi(data.volume);
    }
  } catch (err) {
    setStatus(msgStatus, `Status failed: ${err.message}`, true);
  }
}

function setVolumeUi(value) {
  const v = Math.max(0, Math.min(100, Number(value) || 0));
  volumeSlider.value = String(v);
  volumeValue.textContent = `${v}%`;
}

async function loadVolume() {
  try {
    const resp = await fetch("/api/volume");
    const data = await resp.json();
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    setVolumeUi(data.volume);
    setStatus(volumeStatus, "Volume synced");
  } catch (err) {
    setStatus(volumeStatus, `Volume load failed: ${err.message}`, true);
  }
}

async function pushVolume(value) {
  return pushVolumeWithPersist(value, true);
}

async function pushVolumeWithPersist(value, persist) {
  try {
    const body = new FormData();
    body.append("volume", String(value));
    body.append("persist", persist ? "1" : "0");
    const resp = await fetch("/api/volume", { method: "POST", body });
    const raw = await resp.text();
    if (!resp.ok) throw new Error(raw || `HTTP ${resp.status}`);
    const data = JSON.parse(raw);
    setVolumeUi(data.volume);
    setStatus(volumeStatus, persist
      ? `Volume applied and saved: ${data.volume}%`
      : `Volume preview: ${data.volume}%`);
  } catch (err) {
    setStatus(volumeStatus, `Volume apply failed: ${err.message}`, true);
  }
}

function scheduleVolumePush() {
  if (volumePushTimer) {
    clearTimeout(volumePushTimer);
  }
  volumePushTimer = setTimeout(() => {
    pushVolumeWithPersist(volumeSlider.value, false);
    volumePushTimer = null;
  }, 120);
}

document.getElementById("btnLoadCsv").addEventListener("click", loadCsv);
document.getElementById("btnSaveCsv").addEventListener("click", saveCsv);
document.getElementById("btnSendMsg").addEventListener("click", sendMsg);
document.getElementById("btnClearMsg").addEventListener("click", () => {
  msgBox.value = "";
  msgBox.focus();
});
volumeSlider.addEventListener("input", () => {
  setVolumeUi(volumeSlider.value);
  scheduleVolumePush();
});
volumeSlider.addEventListener("change", () => {
  setVolumeUi(volumeSlider.value);
  pushVolumeWithPersist(volumeSlider.value, true);
});

setInterval(refreshStatus, 1000);
loadCsv();
loadVolume();
refreshStatus();
