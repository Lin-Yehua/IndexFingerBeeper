const csvBox = document.getElementById("csvBox");
const msgBox = document.getElementById("msgBox");
const csvStatus = document.getElementById("csvStatus");
const msgStatus = document.getElementById("msgStatus");
const volumeSlider = document.getElementById("volumeSlider");
const volumeValue = document.getElementById("volumeValue");
const volumeStatus = document.getElementById("volumeStatus");
const hostMacInput = document.getElementById("hostMacInput");
const hostMacStatus = document.getElementById("hostMacStatus");
const apSsidInput = document.getElementById("apSsidInput");
const apPasswordInput = document.getElementById("apPasswordInput");
const apChannelInput = document.getElementById("apChannelInput");
const apConfigStatus = document.getElementById("apConfigStatus");
const selfMacInput = document.getElementById("selfMacInput");
const selfMacStatus = document.getElementById("selfMacStatus");

let volumePushTimer = null;

function setStatus(el, text, isError = false) {
  el.textContent = text || "";
  el.classList.toggle("error", !!isError);
}

async function copyText(text) {
  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch (err) {}

  const ta = document.createElement("textarea");
  ta.value = text;
  ta.style.position = "fixed";
  ta.style.opacity = "0";
  document.body.appendChild(ta);
  ta.focus();
  ta.select();
  let ok = false;
  try {
    ok = document.execCommand("copy");
  } catch (err) {
    ok = false;
  }
  document.body.removeChild(ta);
  return ok;
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
    setStatus(msgStatus, `WebQueue: ${data.queue} | HostQueue: ${data.hostQueue} | CSV reload pending: ${data.csvReloadPending} | AP: ${data.apSsid || "-"} CH:${data.apChannel || "-"}`);
    selfMacInput.value = data.selfMac || "";
    if (typeof data.volume === "number" && document.activeElement !== volumeSlider) {
      setVolumeUi(data.volume);
    }
  } catch (err) {
    setStatus(msgStatus, `Status failed: ${err.message}`, true);
  }
}

async function loadHostMac() {
  try {
    const resp = await fetch("/api/hostmac");
    const data = await resp.json();
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    hostMacInput.value = data.hostMac || "";
    setStatus(hostMacStatus, data.enabled
      ? `Host MAC filter enabled: ${data.hostMac}`
      : "Host MAC filter disabled (accept all)");
  } catch (err) {
    setStatus(hostMacStatus, `Host MAC load failed: ${err.message}`, true);
  }
}

async function saveHostMac() {
  try {
    const body = new FormData();
    body.append("hostMac", hostMacInput.value.trim());
    const resp = await fetch("/api/hostmac", { method: "POST", body });
    const raw = await resp.text();
    if (!resp.ok) throw new Error(raw || `HTTP ${resp.status}`);
    const data = JSON.parse(raw);
    hostMacInput.value = data.hostMac || "";
    setStatus(hostMacStatus, data.enabled
      ? `Host MAC saved: ${data.hostMac}`
      : "Host MAC filter disabled");
  } catch (err) {
    setStatus(hostMacStatus, `Host MAC save failed: ${err.message}`, true);
  }
}

async function loadApConfig() {
  try {
    const resp = await fetch("/api/apconfig");
    const data = await resp.json();
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    apSsidInput.value = data.ssid || "";
    apPasswordInput.value = "";
    apChannelInput.value = String(data.channel || 1);
    setStatus(apConfigStatus, data.passwordSet
      ? "AP password is set on device"
      : "Open AP (no password)");
  } catch (err) {
    setStatus(apConfigStatus, `AP config load failed: ${err.message}`, true);
  }
}

async function saveApConfig() {
  try {
    const body = new FormData();
    body.append("ssid", apSsidInput.value.trim());
    body.append("password", apPasswordInput.value);
    body.append("channel", apChannelInput.value.trim());
    const resp = await fetch("/api/apconfig", { method: "POST", body });
    const raw = await resp.text();
    if (!resp.ok) throw new Error(raw || `HTTP ${resp.status}`);
    const data = JSON.parse(raw);
    setStatus(apConfigStatus, `Saved: SSID=${data.ssid} CH=${data.channel}. Reboot required.`);
    apPasswordInput.value = "";
    await refreshStatus();
  } catch (err) {
    setStatus(apConfigStatus, `AP config save failed: ${err.message}`, true);
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
document.getElementById("btnSaveHostMac").addEventListener("click", saveHostMac);
document.getElementById("btnSaveApConfig").addEventListener("click", saveApConfig);
document.getElementById("btnCopySelfMac").addEventListener("click", async () => {
  const mac = (selfMacInput.value || "").trim();
  if (!mac) {
    setStatus(selfMacStatus, "MAC is empty", true);
    return;
  }
  const ok = await copyText(mac);
  setStatus(selfMacStatus, ok ? "MAC copied" : "Copy failed", !ok);
});

setInterval(refreshStatus, 1000);
loadCsv();
loadVolume();
loadHostMac();
loadApConfig();
refreshStatus();
