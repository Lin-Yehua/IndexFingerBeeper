const csvBox = document.getElementById("csvBox");
const msgBox = document.getElementById("msgBox");
const csvStatus = document.getElementById("csvStatus");
const msgStatus = document.getElementById("msgStatus");
const bgVolumeSlider = document.getElementById("bgVolumeSlider");
const bgVolumeValue = document.getElementById("bgVolumeValue");
const insertVolumeSlider = document.getElementById("insertVolumeSlider");
const insertVolumeValue = document.getElementById("insertVolumeValue");
const volumeStatus = document.getElementById("volumeStatus");
const wrongProb3Input = document.getElementById("wrongProb3Input");
const wrongProb5Input = document.getElementById("wrongProb5Input");
const enableReprintInput = document.getElementById("enableReprintInput");
const backlightInput = document.getElementById("backlightInput");
const backlightTimeInput = document.getElementById("backlightTimeInput");
const effectsStatus = document.getElementById("effectsStatus");
const instantRefreshNoKey = document.getElementById("instantRefreshNoKey");
const refreshModeStatus = document.getElementById("refreshModeStatus");
const hostMacInput = document.getElementById("hostMacInput");
const hostMacStatus = document.getElementById("hostMacStatus");
const apSsidInput = document.getElementById("apSsidInput");
const apPasswordInput = document.getElementById("apPasswordInput");
const apChannelInput = document.getElementById("apChannelInput");
const apConfigStatus = document.getElementById("apConfigStatus");
const selfMacInput = document.getElementById("selfMacInput");
const selfMacStatus = document.getElementById("selfMacStatus");
const imgFileInput = document.getElementById("imgFileInput");
const imgTargetWidthInput = document.getElementById("imgTargetWidthInput");
const imgTargetHeightInput = document.getElementById("imgTargetHeightInput");
const imgCenterXInput = document.getElementById("imgCenterXInput");
const imgCenterYInput = document.getElementById("imgCenterYInput");
const imgCropOffsetYInput = document.getElementById("imgCropOffsetYInput");
const imgPreviewCanvas = document.getElementById("imgPreviewCanvas");
const imgStatus = document.getElementById("imgStatus");
const portalBrowserHint = document.getElementById("portalBrowserHint");
const portalUrlInput = document.getElementById("portalUrlInput");
const btnCopyPortalUrl = document.getElementById("btnCopyPortalUrl");

let volumePushTimer = null;
const IMAGE_FILE_MAX_BYTES = 10 * 1024 * 1024;
let preparedImageBuffer = null;
let selectedImageFile = null;

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

function parseIntInputWithFallback(input, fallback, min, max) {
  const raw = (input.value || "").trim();
  const parsed = Number.parseInt(raw, 10);
  let value = Number.isFinite(parsed) ? parsed : fallback;
  if (Number.isFinite(min)) value = Math.max(min, value);
  if (Number.isFinite(max)) value = Math.min(max, value);
  input.value = String(value);
  return value;
}

function clearImagePreview() {
  if (!imgPreviewCanvas) return;
  const ctx = imgPreviewCanvas.getContext("2d");
  ctx.fillStyle = "#000000";
  ctx.fillRect(0, 0, imgPreviewCanvas.width, imgPreviewCanvas.height);
}

function getPortalRootUrl() {
  return `${window.location.protocol}//${window.location.host}/`;
}

function isLikelyCaptivePortalMiniBrowser() {
  const ua = (navigator.userAgent || "").toLowerCase();
  return ua.includes("captivenetworksupport")
    || ua.includes("wifilogin")
    || ua.includes("captive")
    || ua.includes("micromessenger")
    || ua.includes("wv");
}

function setupPortalBrowserHint() {
  const url = getPortalRootUrl();
  if (portalUrlInput) portalUrlInput.value = url;

  if (portalBrowserHint) {
    portalBrowserHint.textContent = isLikelyCaptivePortalMiniBrowser()
      ? "Current page may be in captive Wi-Fi mini browser where file picker can be blocked. Copy URL and open it in a full browser."
      : "If file picker does not open in captive Wi-Fi popup, copy URL and open it in a full browser.";
  }
}

function loadImageFromFile(file) {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file);
    const img = new Image();
    img.onload = () => {
      URL.revokeObjectURL(url);
      resolve(img);
    };
    img.onerror = () => {
      URL.revokeObjectURL(url);
      reject(new Error("image decode failed"));
    };
    img.src = url;
  });
}

function imageDataToRgb565ArrayBuffer(imageData) {
  const src = imageData.data;
  const pixelCount = imageData.width * imageData.height;
  const out = new ArrayBuffer(pixelCount * 2);
  const view = new DataView(out);
  let srcOffset = 0;
  for (let i = 0; i < pixelCount; i++) {
    const r = src[srcOffset];
    const g = src[srcOffset + 1];
    const b = src[srcOffset + 2];
    const rgb565 = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
    // Swap bytes in conversion stage to match TFT pushImage default byte order.
    const swapped = ((rgb565 & 0xff) << 8) | ((rgb565 >> 8) & 0xff);
    view.setUint16(i * 2, swapped, true);
    srcOffset += 4;
  }
  return out;
}

async function prepareImageBufferFromSelectedFile() {
  if (!selectedImageFile) {
    preparedImageBuffer = null;
    clearImagePreview();
    return;
  }

  const file = selectedImageFile;
  const lowerName = (file.name || "").toLowerCase();
  const looksLikeImage = file.type.startsWith("image/");
  const extOk = lowerName.endsWith(".png")
    || lowerName.endsWith(".jpg")
    || lowerName.endsWith(".jpeg")
    || lowerName.endsWith(".gif");
  if (!looksLikeImage && !extOk) {
    preparedImageBuffer = null;
    setStatus(imgStatus, "Only PNG/JPG/GIF is supported", true);
    return;
  }
  if (file.size > IMAGE_FILE_MAX_BYTES) {
    preparedImageBuffer = null;
    setStatus(imgStatus, "Image file must be <= 10MB", true);
    return;
  }

  const targetW = parseIntInputWithFallback(imgTargetWidthInput, 320, 1, 320);
  const targetH = parseIntInputWithFallback(imgTargetHeightInput, 140, 1, 240);
  const cropOffsetY = parseIntInputWithFallback(imgCropOffsetYInput, 0, -2000, 2000);

  try {
    const img = await loadImageFromFile(file);
    const canvas = imgPreviewCanvas;
    const ctx = canvas.getContext("2d", { willReadFrequently: true });

    canvas.width = targetW;
    canvas.height = targetH;
    ctx.fillStyle = "#000000";
    ctx.fillRect(0, 0, targetW, targetH);
    ctx.imageSmoothingEnabled = true;

    if (img.width >= targetW && img.height >= targetH) {
      const scale = Math.max(targetW / img.width, targetH / img.height);
      const drawW = img.width * scale;
      const drawH = img.height * scale;
      const drawX = (targetW - drawW) * 0.5;
      const minDrawY = targetH - drawH;
      const maxDrawY = 0;
      let drawY = (targetH - drawH) * 0.5 + cropOffsetY;
      if (drawY < minDrawY) drawY = minDrawY;
      if (drawY > maxDrawY) drawY = maxDrawY;
      ctx.drawImage(img, drawX, drawY, drawW, drawH);
    } else {
      const drawX = Math.floor((targetW - img.width) * 0.5);
      const drawY = Math.floor((targetH - img.height) * 0.5);
      ctx.drawImage(img, drawX, drawY, img.width, img.height);
    }

    preparedImageBuffer = imageDataToRgb565ArrayBuffer(ctx.getImageData(0, 0, targetW, targetH));
    setStatus(
      imgStatus,
      `Prepared ${targetW}x${targetH} RGB565 (${preparedImageBuffer.byteLength} bytes), source ${img.width}x${img.height}, cropOffsetY=${cropOffsetY}`
    );
  } catch (err) {
    preparedImageBuffer = null;
    setStatus(imgStatus, `Prepare image failed: ${err.message}`, true);
  }
}

async function sendPreparedImage() {
  if (!preparedImageBuffer) {
    setStatus(imgStatus, "Please choose an image first", true);
    return;
  }

  const targetW = parseIntInputWithFallback(imgTargetWidthInput, 320, 1, 320);
  const targetH = parseIntInputWithFallback(imgTargetHeightInput, 140, 1, 240);
  const centerX = parseIntInputWithFallback(imgCenterXInput, 160, -1024, 1024);
  const centerY = parseIntInputWithFallback(imgCenterYInput, 155, -1024, 1024);

  const expectedBytes = targetW * targetH * 2;
  if (preparedImageBuffer.byteLength !== expectedBytes) {
    setStatus(imgStatus, "Target size changed. Please reselect image.", true);
    return;
  }

  try {
    const params = new URLSearchParams();
    params.set("width", String(targetW));
    params.set("height", String(targetH));
    params.set("centerX", String(centerX));
    params.set("centerY", String(centerY));

    const form = new FormData();
    const blob = new Blob([preparedImageBuffer], { type: "application/octet-stream" });
    form.append("file", blob, "frame.rgb565");

    const resp = await fetch(`/api/image?${params.toString()}`, {
      method: "POST",
      body: form
    });
    const raw = await resp.text();
    if (!resp.ok) throw new Error(raw || `HTTP ${resp.status}`);

    const data = JSON.parse(raw);
    setStatus(
      imgStatus,
      `Image sent: ${data.width}x${data.height}, center=(${data.centerX},${data.centerY})`
    );
    await refreshStatus();
  } catch (err) {
    setStatus(imgStatus, `Send image failed: ${err.message}`, true);
  }
}

function clearPreparedImage() {
  selectedImageFile = null;
  preparedImageBuffer = null;
  if (imgFileInput) imgFileInput.value = "";
  clearImagePreview();
  setStatus(imgStatus, "Image cleared");
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
    setStatus(
      msgStatus,
      `WebQueue: ${data.queue} | HostQueue: ${data.hostQueue} | CSV reload pending: ${data.csvReloadPending} | Image pending: ${data.imagePending} | AP: ${data.apSsid || "-"} CH:${data.apChannel || "-"}`
    );
    selfMacInput.value = data.selfMac || "";
    if (typeof data.instantRefreshNoKey === "boolean" && document.activeElement !== instantRefreshNoKey) {
      instantRefreshNoKey.checked = data.instantRefreshNoKey;
    }
    const editingBg = document.activeElement === bgVolumeSlider;
    const editingInsert = document.activeElement === insertVolumeSlider;
    if (!editingBg && !editingInsert) {
      const insertVolume = Number.isFinite(data.insertVolume) ? data.insertVolume : data.volume;
      const bgVolume = Number.isFinite(data.bgVolume) ? data.bgVolume : data.volume;
      if (Number.isFinite(insertVolume) || Number.isFinite(bgVolume)) {
        setVolumeUi(insertVolume, bgVolume);
      }
    }
  } catch (err) {
    setStatus(msgStatus, `Status failed: ${err.message}`, true);
  }
}

async function loadRefreshMode() {
  try {
    const resp = await fetch("/api/refreshmode");
    const data = await resp.json();
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    instantRefreshNoKey.checked = !!data.instantRefreshNoKey;
    setStatus(refreshModeStatus, instantRefreshNoKey.checked
      ? "Instant refresh enabled"
      : "Wait key press for queued web messages");
  } catch (err) {
    setStatus(refreshModeStatus, `Refresh mode load failed: ${err.message}`, true);
  }
}

async function saveRefreshMode() {
  try {
    const body = new FormData();
    body.append("instantRefreshNoKey", instantRefreshNoKey.checked ? "1" : "0");
    const resp = await fetch("/api/refreshmode", { method: "POST", body });
    const raw = await resp.text();
    if (!resp.ok) throw new Error(raw || `HTTP ${resp.status}`);
    const data = JSON.parse(raw);
    instantRefreshNoKey.checked = !!data.instantRefreshNoKey;
    setStatus(refreshModeStatus, instantRefreshNoKey.checked
      ? "Instant refresh saved"
      : "Key-gated refresh saved");
  } catch (err) {
    setStatus(refreshModeStatus, `Refresh mode save failed: ${err.message}`, true);
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

async function loadEffects() {
  try {
    const resp = await fetch("/api/effects");
    const data = await resp.json();
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);

    wrongProb3Input.value = String(data.wrongProb3 ?? 25);
    wrongProb5Input.value = String(data.wrongProb5 ?? 12);
    enableReprintInput.checked = !!data.enableReprint;
    backlightInput.value = String(data.backlight ?? 1);
    backlightTimeInput.value = String(data.backlightTime ?? -1);

    setStatus(
      effectsStatus,
      `Loaded: P3=${wrongProb3Input.value} P5=${wrongProb5Input.value} Reprint=${enableReprintInput.checked ? "on" : "off"} Backlight=${backlightInput.value} BacklightTime=${backlightTimeInput.value}`
    );
  } catch (err) {
    setStatus(effectsStatus, `Effects load failed: ${err.message}`, true);
  }
}

async function saveEffects() {
  const wrong3 = Number.parseInt((wrongProb3Input.value || "").trim(), 10);
  const wrong5 = Number.parseInt((wrongProb5Input.value || "").trim(), 10);
  const backlight = Number.parseFloat((backlightInput.value || "").trim());
  const backlightTime = Number.parseInt((backlightTimeInput.value || "").trim(), 10);

  if (!Number.isFinite(wrong3) || wrong3 < 0 || wrong3 > 100) {
    setStatus(effectsStatus, "P3 must be 0-100", true);
    return;
  }
  if (!Number.isFinite(wrong5) || wrong5 < 0 || wrong5 > 100) {
    setStatus(effectsStatus, "P5 must be 0-100", true);
    return;
  }
  if (!Number.isFinite(backlight) || backlight < 0 || backlight > 1) {
    setStatus(effectsStatus, "Backlight must be 0.0-1.0", true);
    return;
  }
  if (!Number.isFinite(backlightTime)) {
    setStatus(effectsStatus, "BacklightTime must be integer", true);
    return;
  }

  try {
    const body = new FormData();
    body.append("wrongProb3", String(wrong3));
    body.append("wrongProb5", String(wrong5));
    body.append("enableReprint", enableReprintInput.checked ? "1" : "0");
    body.append("backlight", String(backlight));
    body.append("backlightTime", String(backlightTime));

    const resp = await fetch("/api/effects", { method: "POST", body });
    const raw = await resp.text();
    if (!resp.ok) throw new Error(raw || `HTTP ${resp.status}`);
    const data = JSON.parse(raw);

    wrongProb3Input.value = String(data.wrongProb3);
    wrongProb5Input.value = String(data.wrongProb5);
    enableReprintInput.checked = !!data.enableReprint;
    backlightInput.value = String(data.backlight);
    backlightTimeInput.value = String(data.backlightTime);

    setStatus(
      effectsStatus,
      `Saved: P3=${data.wrongProb3} P5=${data.wrongProb5} Reprint=${data.enableReprint ? "on" : "off"} Backlight=${data.backlight} BacklightTime=${data.backlightTime}`
    );
  } catch (err) {
    setStatus(effectsStatus, `Effects save failed: ${err.message}`, true);
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

function clampPercent(value, fallback = 0) {
  const n = Number(value);
  if (!Number.isFinite(n)) return fallback;
  return Math.max(0, Math.min(100, Math.round(n)));
}

function setVolumeUi(insertValue, bgValue) {
  const insert = clampPercent(insertValue, clampPercent(insertVolumeSlider.value, 20));
  const bg = clampPercent(bgValue, clampPercent(bgVolumeSlider.value, 20));
  insertVolumeSlider.value = String(insert);
  bgVolumeSlider.value = String(bg);
  insertVolumeValue.textContent = `${insert}%`;
  bgVolumeValue.textContent = `${bg}%`;
}

async function loadVolume() {
  try {
    const resp = await fetch("/api/volume");
    const data = await resp.json();
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    const insertVolume = Number.isFinite(data.insertVolume) ? data.insertVolume : data.volume;
    const bgVolume = Number.isFinite(data.bgVolume) ? data.bgVolume : data.volume;
    setVolumeUi(insertVolume, bgVolume);
    setStatus(volumeStatus, "Volume synced (insert + background)");
  } catch (err) {
    setStatus(volumeStatus, `Volume load failed: ${err.message}`, true);
  }
}

async function pushVolumeWithPersist(insertValue, bgValue, persist) {
  try {
    const insert = clampPercent(insertValue, 0);
    const bg = clampPercent(bgValue, 0);
    const body = new FormData();
    body.append("insertVolume", String(insert));
    body.append("bgVolume", String(bg));
    body.append("persist", persist ? "1" : "0");
    const resp = await fetch("/api/volume", { method: "POST", body });
    const raw = await resp.text();
    if (!resp.ok) throw new Error(raw || `HTTP ${resp.status}`);
    const data = JSON.parse(raw);
    setVolumeUi(data.insertVolume, data.bgVolume);
    setStatus(volumeStatus, persist
      ? `Saved: Insert ${data.insertVolume}% | BG ${data.bgVolume}%`
      : `Preview: Insert ${data.insertVolume}% | BG ${data.bgVolume}%`);
  } catch (err) {
    setStatus(volumeStatus, `Volume apply failed: ${err.message}`, true);
  }
}

function scheduleVolumePush() {
  if (volumePushTimer) {
    clearTimeout(volumePushTimer);
  }
  volumePushTimer = setTimeout(() => {
    pushVolumeWithPersist(insertVolumeSlider.value, bgVolumeSlider.value, false);
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
instantRefreshNoKey.addEventListener("change", saveRefreshMode);
bgVolumeSlider.addEventListener("input", () => {
  setVolumeUi(insertVolumeSlider.value, bgVolumeSlider.value);
  scheduleVolumePush();
});
insertVolumeSlider.addEventListener("input", () => {
  setVolumeUi(insertVolumeSlider.value, bgVolumeSlider.value);
  scheduleVolumePush();
});
bgVolumeSlider.addEventListener("change", () => {
  setVolumeUi(insertVolumeSlider.value, bgVolumeSlider.value);
  pushVolumeWithPersist(insertVolumeSlider.value, bgVolumeSlider.value, true);
});
insertVolumeSlider.addEventListener("change", () => {
  setVolumeUi(insertVolumeSlider.value, bgVolumeSlider.value);
  pushVolumeWithPersist(insertVolumeSlider.value, bgVolumeSlider.value, true);
});
document.getElementById("btnSaveHostMac").addEventListener("click", saveHostMac);
document.getElementById("btnSaveApConfig").addEventListener("click", saveApConfig);
document.getElementById("btnSaveEffects").addEventListener("click", saveEffects);
document.getElementById("btnSendImage").addEventListener("click", sendPreparedImage);
document.getElementById("btnClearImage").addEventListener("click", clearPreparedImage);
btnCopyPortalUrl.addEventListener("click", async () => {
  const ok = await copyText(getPortalRootUrl());
  setStatus(portalBrowserHint, ok ? "Portal URL copied" : "Copy URL failed", !ok);
});
imgFileInput.addEventListener("change", async (ev) => {
  const file = ev.target.files && ev.target.files[0] ? ev.target.files[0] : null;
  selectedImageFile = file;
  if (!file) {
    preparedImageBuffer = null;
    clearImagePreview();
    setStatus(imgStatus, "");
    return;
  }
  await prepareImageBufferFromSelectedFile();
});
imgTargetWidthInput.addEventListener("change", prepareImageBufferFromSelectedFile);
imgTargetHeightInput.addEventListener("change", prepareImageBufferFromSelectedFile);
imgCropOffsetYInput.addEventListener("change", prepareImageBufferFromSelectedFile);
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
loadRefreshMode();
loadHostMac();
loadApConfig();
loadEffects();
setupPortalBrowserHint();
clearImagePreview();
refreshStatus();
