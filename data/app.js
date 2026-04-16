const $ = (id) => document.getElementById(id);
const st = {
  msgHistory: [],
  commands: [],
  schedules: [],
  imageFile: null,
  imageBuf: null,
  volTimer: null
};
const LS = { hist: "bb_msg_history", pfxOn: "bb_prefix_on", pfxText: "bb_prefix_text" };

const el = {
  tabMsg: $("tabMsg"), tabImg: $("tabImg"), panelMsg: $("panelMsg"), panelImg: $("panelImg"),
  msgInput: $("msgInput"), msgHistory: $("msgHistory"), msgImmediate: $("msgImmediate"),
  msgPrefixEnable: $("msgPrefixEnable"), msgPrefix: $("msgPrefix"), btnSendMsg: $("btnSendMsg"),
  btnClearMsg: $("btnClearMsg"), msgStatus: $("msgStatus"),
  portalUrl: $("portalUrl"), btnCopyPortalUrl: $("btnCopyPortalUrl"), imgFile: $("imgFile"),
  imgOffsetY: $("imgOffsetY"), imgPreview: $("imgPreview"), imgFitMode: $("imgFitMode"),
  imgCenterX: $("imgCenterX"), imgCenterY: $("imgCenterY"), imgWidth: $("imgWidth"), imgHeight: $("imgHeight"),
  btnSendImg: $("btnSendImg"), btnClearImg: $("btnClearImg"), imgStatus: $("imgStatus"),
  cmdList: $("cmdList"), cmdAddInput: $("cmdAddInput"), btnCmdAdd: $("btnCmdAdd"), btnCmdSave: $("btnCmdSave"), cmdStatus: $("cmdStatus"),
  rtcNow: $("rtcNow"), rtcYear: $("rtcYear"), rtcMonth: $("rtcMonth"), rtcDay: $("rtcDay"), rtcHour: $("rtcHour"), rtcMinute: $("rtcMinute"), rtcSecond: $("rtcSecond"), btnRtcSet: $("btnRtcSet"), btnRtcSyncPhone: $("btnRtcSyncPhone"), rtcStatus: $("rtcStatus"),
  scheduleDate: $("scheduleDate"), scheduleList: $("scheduleList"), scheduleAddTime: $("scheduleAddTime"), scheduleAddRepeat: $("scheduleAddRepeat"), scheduleAddText: $("scheduleAddText"), btnScheduleAdd: $("btnScheduleAdd"), btnScheduleSave: $("btnScheduleSave"), scheduleStatus: $("scheduleStatus"),
  bgVol: $("bgVol"), insertVol: $("insertVol"), bgVolVal: $("bgVolVal"), insertVolVal: $("insertVolVal"), backlight: $("backlight"), backlightTime: $("backlightTime"), backlightCloseTime: $("backlightCloseTime"), btnSaveDisplay: $("btnSaveDisplay"), displayStatus: $("displayStatus"),
  apSsid: $("apSsid"), apPassword: $("apPassword"), apChannel: $("apChannel"), btnSaveAp: $("btnSaveAp"),
  staSsid: $("staSsid"), staPassword: $("staPassword"), staNet: $("staNet"), btnSaveSta: $("btnSaveSta"),
  hostMac: $("hostMac"), btnSaveHostMac: $("btnSaveHostMac"), selfMac: $("selfMac"), btnCopySelfMac: $("btnCopySelfMac"), wirelessStatus: $("wirelessStatus"),
  batteryStatus: $("batteryStatus")
};

const pad2 = (n) => String(n).padStart(2, "0");
const toInt = (v, d = 0) => { const n = parseInt(String(v ?? "").trim(), 10); return Number.isFinite(n) ? n : d; };
const clamp = (n, a, b) => Math.max(a, Math.min(b, n));
const setStatus = (node, text, err = false) => { if (!node) return; node.textContent = text || ""; node.classList.toggle("error", !!err); node.classList.toggle("ok", !err && !!text); };
const autoGrow = (ta) => { if (!ta) return; ta.style.height = "auto"; ta.style.height = `${ta.scrollHeight + 2}px`; };

async function fetchJsonSafe(url, label) {
  const r = await fetch(url);
  const raw = await r.text();
  if (!raw || !raw.trim()) {
    throw new Error(`${label} 返回空响应 (HTTP ${r.status})`);
  }
  let d = null;
  try {
    d = JSON.parse(raw);
  } catch (e) {
    const snippet = raw.length > 120 ? `${raw.slice(0, 120)}...` : raw;
    throw new Error(`${label} 返回非JSON: ${e.message} | 内容: ${snippet}`);
  }
  if (!r.ok) {
    const detail = d && (d.message || d.error) ? (d.message || d.error) : raw;
    throw new Error(`${label} HTTP ${r.status}: ${detail}`);
  }
  return d;
}

const delayMs = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function fetchJsonSafeRetry(url, label, attempts = 2) {
  let lastErr = null;
  for (let i = 0; i < attempts; i += 1) {
    try {
      return await fetchJsonSafe(url, label);
    } catch (e) {
      lastErr = e;
      if (i + 1 < attempts) {
        await delayMs(120);
      }
    }
  }
  throw lastErr || new Error(`${label} 请求失败`);
}

async function copyText(t) {
  try { if (navigator.clipboard && window.isSecureContext) { await navigator.clipboard.writeText(t); return true; } } catch (_) {}
  const ta = document.createElement("textarea"); ta.value = t; ta.style.position = "fixed"; ta.style.opacity = "0"; document.body.appendChild(ta);
  ta.focus(); ta.select(); let ok = false; try { ok = document.execCommand("copy"); } catch (_) {} document.body.removeChild(ta); return ok;
}

function switchPush(mode) {
  const m = mode === "msg";
  el.tabMsg.classList.toggle("active", m); el.tabImg.classList.toggle("active", !m);
  el.panelMsg.classList.toggle("active", m); el.panelImg.classList.toggle("active", !m);
}

function loadMsgLocal() {
  try {
    const raw = JSON.parse(localStorage.getItem(LS.hist) || "[]");
    if (Array.isArray(raw)) {
      st.msgHistory = raw.map((x) => {
        if (typeof x === "string") return { msg: x.trim(), prefix: "" };
        const msg = String(x?.msg || x?.message || "").trim();
        const prefix = String(x?.prefix || "").trim();
        return { msg, prefix };
      }).filter((x) => x.msg);
    } else {
      st.msgHistory = [];
    }
  } catch (_) { st.msgHistory = []; }
  el.msgPrefixEnable.checked = localStorage.getItem(LS.pfxOn) === "1";
  el.msgPrefix.value = localStorage.getItem(LS.pfxText) || "";
  renderMsgHistory();
}
function saveMsgLocal() {
  localStorage.setItem(LS.hist, JSON.stringify(st.msgHistory.slice(0, 30)));
  localStorage.setItem(LS.pfxOn, el.msgPrefixEnable.checked ? "1" : "0");
  localStorage.setItem(LS.pfxText, el.msgPrefix.value || "");
}
function renderMsgHistory() {
  el.msgHistory.innerHTML = "";
  const first = document.createElement("option"); first.value = ""; first.textContent = "历史发送（选择回填）"; el.msgHistory.appendChild(first);
  st.msgHistory.forEach((item, idx) => {
    const o = document.createElement("option");
    o.value = String(idx);
    const head = item.prefix ? `[前缀:${item.prefix}] ` : "";
    const shown = `${head}${item.msg}`;
    o.textContent = shown.length > 58 ? `${shown.slice(0, 58)}...` : shown;
    el.msgHistory.appendChild(o);
  });
}
function recordMsg(msg, prefix) {
  const m = String(msg || "").trim();
  const p = String(prefix || "").trim();
  if (!m) return;
  st.msgHistory = [{ msg: m, prefix: p }, ...st.msgHistory.filter((x) => x.msg !== m || x.prefix !== p)].slice(0, 30);
  saveMsgLocal();
  renderMsgHistory();
}

async function sendMsg() {
  const rawMsg = String(el.msgInput.value || "").replace(/\r?\n+/g, " ").trim();
  if (!rawMsg) return setStatus(el.msgStatus, "请输入消息", true);
  const pfx = String(el.msgPrefix.value || "").replace(/\r?\n+/g, " ").trim();
  const text = (el.msgPrefixEnable.checked && pfx) ? `${pfx}${rawMsg}` : rawMsg;
  try {
    const b = new FormData(); b.append("text", text); b.append("immediate", el.msgImmediate.checked ? "1" : "0");
    const r = await fetch("/api/send", { method: "POST", body: b }); const raw = await r.text(); if (!r.ok) throw new Error(raw || `HTTP ${r.status}`);
    recordMsg(rawMsg, (el.msgPrefixEnable.checked && pfx) ? pfx : "");
    el.msgInput.value = "";
    autoGrow(el.msgInput);
    setStatus(el.msgStatus, raw || "发送成功");
  } catch (e) { setStatus(el.msgStatus, `发送失败: ${e.message}`, true); }
}

function portalRoot() { return `${location.protocol}//${location.host}/`; }
function clearPreview() { const c = el.imgPreview.getContext("2d"); c.fillStyle = "#000"; c.fillRect(0, 0, el.imgPreview.width, el.imgPreview.height); }
function fileToImage(f) { return new Promise((res, rej) => { const u = URL.createObjectURL(f); const i = new Image(); i.onload = () => { URL.revokeObjectURL(u); res(i); }; i.onerror = () => { URL.revokeObjectURL(u); rej(new Error("图片解码失败")); }; i.src = u; }); }
function to565(imgData) {
  const s = imgData.data; const n = imgData.width * imgData.height; const out = new ArrayBuffer(n * 2); const dv = new DataView(out);
  for (let i = 0, j = 0; i < n; i += 1, j += 4) { const r = s[j], g = s[j + 1], b = s[j + 2]; const v = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3); const sw = ((v & 0xff) << 8) | ((v >> 8) & 0xff); dv.setUint16(i * 2, sw, true); }
  return out;
}

async function prepImage() {
  if (!st.imageFile) { st.imageBuf = null; clearPreview(); return; }
  try {
    const w = clamp(toInt(el.imgWidth.value, 320), 1, 320), h = clamp(toInt(el.imgHeight.value, 140), 1, 240), oy = clamp(toInt(el.imgOffsetY.value, 0), -200, 200);
    const img = await fileToImage(st.imageFile); const fit = el.imgFitMode.value; el.imgPreview.width = w; el.imgPreview.height = h;
    const ctx = el.imgPreview.getContext("2d", { willReadFrequently: true }); ctx.fillStyle = "#000"; ctx.fillRect(0, 0, w, h);
    if (fit === "center") { const sc = Math.min(w / img.width, h / img.height, 1); const dw = img.width * sc, dh = img.height * sc; ctx.drawImage(img, (w - dw) / 2, (h - dh) / 2 + oy, dw, dh); }
    else { const sc = Math.max(w / img.width, h / img.height); const dw = img.width * sc, dh = img.height * sc; let dy = (h - dh) / 2 + oy; dy = Math.max(h - dh, Math.min(0, dy)); ctx.drawImage(img, (w - dw) / 2, dy, dw, dh); }
    st.imageBuf = to565(ctx.getImageData(0, 0, w, h)); setStatus(el.imgStatus, `已准备 ${w}x${h}`);
  } catch (e) { st.imageBuf = null; setStatus(el.imgStatus, `图片处理失败: ${e.message}`, true); }
}

async function sendImage() {
  if (!st.imageBuf) return setStatus(el.imgStatus, "请先选择图片", true);
  const w = clamp(toInt(el.imgWidth.value, 320), 1, 320), h = clamp(toInt(el.imgHeight.value, 140), 1, 240);
  if (st.imageBuf.byteLength !== w * h * 2) return setStatus(el.imgStatus, "尺寸变化，请重选图片", true);
  try {
    const p = new URLSearchParams(); p.set("width", w); p.set("height", h); p.set("centerX", clamp(toInt(el.imgCenterX.value, 160), -1024, 1024)); p.set("centerY", clamp(toInt(el.imgCenterY.value, 155), -1024, 1024));
    const b = new FormData(); b.append("file", new Blob([st.imageBuf], { type: "application/octet-stream" }), "frame.rgb565");
    const r = await fetch(`/api/image?${p.toString()}`, { method: "POST", body: b }); const raw = await r.text(); if (!r.ok) throw new Error(raw || `HTTP ${r.status}`);
    const d = JSON.parse(raw); setStatus(el.imgStatus, `发送成功 ${d.width}x${d.height}`);
  } catch (e) { setStatus(el.imgStatus, `发送失败: ${e.message}`, true); }
}
function parseCmdLine(line) {
  const c = line.indexOf(","); if (c < 0) return null;
  let t = line.slice(c + 1).trim(); if (t.startsWith('"') && t.endsWith('"') && t.length >= 2) t = t.slice(1, -1).replace(/""/g, '"');
  return t.replace(/\r/g, "").trim();
}
function cmdToLine(text, id) { return `${id},"${String(text || "").replace(/\r?\n+/g, " ").replace(/"/g, '""')}"`; }

function renderCmdList() {
  el.cmdList.innerHTML = "";
  if (!st.commands.length) { const p = document.createElement("p"); p.className = "status"; p.textContent = "指令库为空"; el.cmdList.appendChild(p); return; }
  st.commands.forEach((txt, idx) => {
    const wrap = document.createElement("div"); wrap.className = "list-item";
    const head = document.createElement("div"); head.className = "item-head";
    const n = document.createElement("div"); n.textContent = `#${idx + 1}`;
    const del = document.createElement("button"); del.className = "del"; del.type = "button"; del.textContent = "-";
    del.addEventListener("click", () => { st.commands.splice(idx, 1); renderCmdList(); });
    head.append(n, del);
    const ta = document.createElement("textarea"); ta.rows = 2; ta.value = txt; autoGrow(ta);
    ta.addEventListener("input", () => { st.commands[idx] = ta.value; autoGrow(ta); });
    ta.addEventListener("blur", () => { if (!String(ta.value || "").trim()) { st.commands.splice(idx, 1); renderCmdList(); } });
    wrap.append(head, ta); el.cmdList.appendChild(wrap);
  });
}

async function loadCommands() {
  try {
    const r = await fetch("/api/csv"); const raw = await r.text(); if (!r.ok) throw new Error(raw || `HTTP ${r.status}`);
    st.commands = raw.split(/\r?\n/).map((l) => l.trim()).filter((l) => l && !l.startsWith("#")).map(parseCmdLine).filter((x) => typeof x === "string" && x.trim());
    renderCmdList(); setStatus(el.cmdStatus, "指令库已加载");
  } catch (e) { setStatus(el.cmdStatus, `加载失败: ${e.message}`, true); }
}

async function saveCommands() {
  try {
    const lines = st.commands.map((t) => String(t || "").replace(/\r?\n+/g, " ").trim()).filter(Boolean).map((t, i) => cmdToLine(t, i + 1));
    const b = new FormData(); b.append("content", lines.join("\n") + (lines.length ? "\n" : ""));
    const r = await fetch("/api/csv", { method: "POST", body: b }); const raw = await r.text(); if (!r.ok) throw new Error(raw || `HTTP ${r.status}`);
    await loadCommands();
    setStatus(el.cmdStatus, raw || "保存成功");
  } catch (e) { setStatus(el.cmdStatus, `保存失败: ${e.message}`, true); }
}

function splitSchedule(line) {
  const cols = []; let s = 0;
  for (let i = 0; i < 11; i += 1) { const c = line.indexOf(",", s); if (c < 0) { cols.push(line.slice(s).trim()); s = line.length; break; } cols.push(line.slice(s, c).trim()); s = c + 1; }
  if (cols.length < 11) return null; const tail = line.slice(s).trim(); if (tail) cols.push(tail); return cols;
}
function parseScheduleMessageCell(cell) {
  let t = String(cell || "").trim();
  if (t.startsWith("\"") && t.endsWith("\"") && t.length >= 2) {
    t = t.slice(1, -1).replace(/""/g, "\"");
  }
  return t.replace(/\r?\n+/g, " ").trim();
}
function encodeScheduleMessageCell(text) {
  const t = String(text || "").replace(/\r?\n+/g, " ").trim();
  return `"${t.replace(/"/g, "\"\"")}"`;
}
const weekMon = (d) => (d.getDay() === 0 ? 7 : d.getDay());
function schRepeat(it) { if (it.d) return "daily"; if (it.w) return "weekly"; if (it.m) return "monthly"; if (it.y) return "yearly"; return "none"; }
function setSchRepeat(it, type, day) {
  it.d = it.m = it.w = it.y = false;
  if (type === "daily") it.d = true;
  if (type === "weekly") { it.w = true; it.week = weekMon(day); }
  if (type === "monthly") it.m = true;
  if (type === "yearly") it.y = true;
  if (type === "none") { it.Y = day.getFullYear(); it.M = day.getMonth() + 1; it.D = day.getDate(); }
}
function schPrefix(it) { if (it.d) return "日重复"; if (it.w) return "周重复"; if (it.m) return "月重复"; if (it.y) return "年重复"; return "不重复"; }
function schTime(it) { return `${pad2(it.h)}:${pad2(it.i)}`; }

function parseSchedules(text) {
  const out = [];
  text.split(/\r?\n/).forEach((raw, idx) => {
    const line = raw.trim(); if (!line || line.startsWith("#") || line.startsWith(";")) return;
    const c = splitSchedule(line); if (!c) return;
    const Y = toInt(c[0], -1), M = toInt(c[1], -1), D = toInt(c[2], -1), h = toInt(c[3], 0), i = toInt(c[4], 0);
    if (Y < 2000 || Y > 2099 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 23 || i < 0 || i > 59) return;
    const dt = new Date(Y, M - 1, D);
    const wkRaw = toInt(c[6], 0);
    const wk = (wkRaw >= 1 && wkRaw <= 7) ? wkRaw : weekMon(dt);
    out.push({ id: `sc_${Date.now()}_${idx}_${Math.random().toString(16).slice(2, 6)}`, Y, M, D, h, i, week: wk, d: toInt(c[7], 0) !== 0, m: toInt(c[8], 0) !== 0, w: toInt(c[9], 0) !== 0, y: toInt(c[10], 0) !== 0, text: parseScheduleMessageCell(c[11] || "") });
  });
  return out;
}

function schedulesToCsv(list) {
  const lines = ["#YEAR,MOUTH,DAY,HOUR,MIN,SEC,WEEK,IsDayRange,IsMouthRange,IsWeekRange,IsYearRange,ScheduleMessage"];
  [...list].sort((a, b) => (a.Y - b.Y) || (a.M - b.M) || (a.D - b.D) || (a.h - b.h) || (a.i - b.i)).forEach((it) => {
    const line = `${it.Y},${it.M},${it.D},${it.h},${it.i},0,${it.week},${it.d ? 1 : 0},${it.m ? 1 : 0},${it.w ? 1 : 0},${it.y ? 1 : 0},${encodeScheduleMessageCell(it.text)}`;
    lines.push(line);
  });
  return `${lines.join("\n")}\n`;
}

function pickDay() {
  const v = el.scheduleDate.value; if (!v) return new Date();
  const p = v.split("-").map((x) => parseInt(x, 10)); if (p.length !== 3 || p.some((n) => !Number.isFinite(n))) return new Date();
  return new Date(p[0], p[1] - 1, p[2]);
}
function schMatch(it, day) {
  const Y = day.getFullYear(), M = day.getMonth() + 1, D = day.getDate(), w = weekMon(day);
  if (it.d) return true; if (it.w && it.week === w) return true; if (it.m && it.D === D) return true; if (it.y && it.M === M && it.D === D) return true;
  return it.Y === Y && it.M === M && it.D === D;
}
function schPast(it, day) {
  const n = new Date(); const t0 = new Date(n.getFullYear(), n.getMonth(), n.getDate()); const d0 = new Date(day.getFullYear(), day.getMonth(), day.getDate());
  if (d0 < t0) return true; if (d0 > t0) return false;
  return (it.h * 60 + it.i) < (n.getHours() * 60 + n.getMinutes());
}
function renderSchedules() {
  el.scheduleList.innerHTML = "";
  const day = pickDay();
  const shown = st.schedules.filter((it) => schMatch(it, day)).sort((a, b) => (a.h - b.h) || (a.i - b.i));
  if (!shown.length) { const p = document.createElement("p"); p.className = "status"; p.textContent = "当日无日程"; el.scheduleList.appendChild(p); return; }

  shown.forEach((it) => {
    const wrap = document.createElement("div"); wrap.className = "list-item"; if (schPast(it, day)) wrap.classList.add("is-past");
    const head = document.createElement("div"); head.className = "item-head";
    const lab = document.createElement("div"); lab.className = "item-repeat"; lab.textContent = schPrefix(it);
    const del = document.createElement("button"); del.className = "del"; del.type = "button"; del.textContent = "-";
    del.addEventListener("click", () => { st.schedules = st.schedules.filter((x) => x.id !== it.id); renderSchedules(); });
    head.append(lab, del);

    const row = document.createElement("div"); row.className = "item-row";
    const ti = document.createElement("input"); ti.type = "time"; ti.value = schTime(it);
    ti.addEventListener("change", () => { const [h, m] = ti.value.split(":").map((v) => toInt(v, 0)); it.h = clamp(h, 0, 23); it.i = clamp(m, 0, 59); renderSchedules(); });

    const rp = document.createElement("select");
    [["none", "不重复"], ["daily", "每日重复"], ["weekly", "每周重复"], ["monthly", "每月重复"], ["yearly", "每年重复"]].forEach(([v, t]) => { const o = document.createElement("option"); o.value = v; o.textContent = t; rp.appendChild(o); });
    rp.value = schRepeat(it);
    rp.addEventListener("change", () => { setSchRepeat(it, rp.value, day); renderSchedules(); });

    const tx = document.createElement("textarea"); tx.rows = 2; tx.placeholder = "内容（可选）"; tx.value = it.text || ""; autoGrow(tx);
    tx.addEventListener("input", () => { it.text = tx.value.replace(/\r?\n+/g, " ").trim(); autoGrow(tx); });

    row.append(ti, rp, tx); wrap.append(head, row); el.scheduleList.appendChild(wrap);
  });
}

async function loadSchedules() {
  try {
    const r = await fetch("/api/schedule"); const raw = await r.text(); if (!r.ok) throw new Error(raw || `HTTP ${r.status}`);
    st.schedules = parseSchedules(raw); renderSchedules(); setStatus(el.scheduleStatus, "日程表已加载");
  } catch (e) { setStatus(el.scheduleStatus, `加载失败: ${e.message}`, true); }
}

async function saveSchedules() {
  try {
    const b = new FormData(); b.append("content", schedulesToCsv(st.schedules));
    const r = await fetch("/api/schedule", { method: "POST", body: b }); const raw = await r.text(); if (!r.ok) throw new Error(raw || `HTTP ${r.status}`);
    await loadSchedules();
    setStatus(el.scheduleStatus, raw || "日程已保存");
  } catch (e) { setStatus(el.scheduleStatus, `保存失败: ${e.message}`, true); }
}

function addSchedule() {
  const d = pickDay(); const [h, m] = (el.scheduleAddTime.value || "09:00").split(":").map((x) => toInt(x, 0));
  const it = { id: `sc_${Date.now()}_${Math.random().toString(16).slice(2, 6)}`, Y: d.getFullYear(), M: d.getMonth() + 1, D: d.getDate(), h: clamp(h, 0, 23), i: clamp(m, 0, 59), week: weekMon(d), d: false, m: false, w: false, y: false, text: String(el.scheduleAddText.value || "").replace(/\r?\n+/g, " ").trim() };
  setSchRepeat(it, el.scheduleAddRepeat.value, d); st.schedules.push(it); el.scheduleAddText.value = ""; renderSchedules(); setStatus(el.scheduleStatus, "已添加（记得保存）");
}

function rtcPayload() {
  const b = new FormData();
  b.append("year", String(clamp(toInt(el.rtcYear.value, 2026), 2000, 2099)));
  b.append("month", String(clamp(toInt(el.rtcMonth.value, 1), 1, 12)));
  b.append("day", String(clamp(toInt(el.rtcDay.value, 1), 1, 31)));
  b.append("hour", String(clamp(toInt(el.rtcHour.value, 0), 0, 23)));
  b.append("minute", String(clamp(toInt(el.rtcMinute.value, 0), 0, 59)));
  b.append("second", String(clamp(toInt(el.rtcSecond.value, 0), 0, 59)));
  return b;
}
function rtcFill(d) { el.rtcYear.value = d.year; el.rtcMonth.value = d.month; el.rtcDay.value = d.day; el.rtcHour.value = d.hour; el.rtcMinute.value = d.minute; el.rtcSecond.value = d.second; }
function rtcText(d) { return `当前RTC: ${d.year}-${pad2(d.month)}-${pad2(d.day)} ${pad2(d.hour)}:${pad2(d.minute)}:${pad2(d.second)} (W${d.week})`; }

async function loadRtc() {
  try {
    const r = await fetch("/api/rtc");
    const d = await r.json();
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    if (!d.ok) throw new Error("RTC读取失败");
    el.rtcNow.textContent = rtcText(d);
    rtcFill(d);
    renderSchedules();
  }
  catch (e) { setStatus(el.rtcStatus, `RTC读取失败: ${e.message}`, true); }
}
async function rtcSet() {
  try {
    const r = await fetch("/api/rtc/set", { method: "POST", body: rtcPayload() });
    const raw = await r.text();
    if (!r.ok) throw new Error(raw || `HTTP ${r.status}`);
    const d = JSON.parse(raw);
    if (!d.ok) throw new Error("写后读失败");
    el.rtcNow.textContent = rtcText(d);
    rtcFill(d);
    renderSchedules();
    setStatus(el.rtcStatus, "RTC设置成功");
  }
  catch (e) { setStatus(el.rtcStatus, `RTC设置失败: ${e.message}`, true); }
}
async function rtcSyncPhone() {
  const n = new Date(); el.rtcYear.value = n.getFullYear(); el.rtcMonth.value = n.getMonth() + 1; el.rtcDay.value = n.getDate(); el.rtcHour.value = n.getHours(); el.rtcMinute.value = n.getMinutes(); el.rtcSecond.value = n.getSeconds();
  try {
    const r = await fetch("/api/rtc/sync-phone", { method: "POST", body: rtcPayload() });
    const raw = await r.text();
    if (!r.ok) throw new Error(raw || `HTTP ${r.status}`);
    const d = JSON.parse(raw);
    if (!d.ok) throw new Error("写后读失败");
    el.rtcNow.textContent = rtcText(d);
    rtcFill(d);
    renderSchedules();
    setStatus(el.rtcStatus, "已同步手机时间");
  }
  catch (e) { setStatus(el.rtcStatus, `同步失败: ${e.message}`, true); }
}

function setVolUi(i, b) {
  i = clamp(toInt(i, 20), 0, 100); b = clamp(toInt(b, 20), 0, 100);
  el.insertVol.value = String(i); el.bgVol.value = String(b); el.insertVolVal.textContent = `${i}%`; el.bgVolVal.textContent = `${b}%`;
}
async function pushVol(persist) {
  const b = new FormData(); b.append("insertVolume", String(clamp(toInt(el.insertVol.value, 20), 0, 100))); b.append("bgVolume", String(clamp(toInt(el.bgVol.value, 20), 0, 100))); b.append("persist", persist ? "1" : "0");
  const r = await fetch("/api/volume", { method: "POST", body: b }); const raw = await r.text(); if (!r.ok) throw new Error(raw || `HTTP ${r.status}`); return JSON.parse(raw);
}
async function loadDisplayAudio() {
  try {
    const [vr, fr] = await Promise.all([fetch("/api/volume"), fetch("/api/effects")]); const vd = await vr.json(); const fd = await fr.json();
    if (vr.ok) setVolUi(Number.isFinite(vd.insertVolume) ? vd.insertVolume : vd.volume, Number.isFinite(vd.bgVolume) ? vd.bgVolume : vd.volume);
    if (fr.ok) { el.backlight.value = String(fd.backlight ?? 1); el.backlightTime.value = String(fd.backlightTime ?? -1); el.backlightCloseTime.value = String(fd.backlightCloseTime ?? 20); }
    setStatus(el.displayStatus, "显示与音量参数已同步");
  } catch (e) { setStatus(el.displayStatus, `加载失败: ${e.message}`, true); }
}
async function saveDisplay() {
  try {
    const b = new FormData(); b.append("backlight", String(parseFloat(el.backlight.value || "1"))); b.append("backlightTime", String(toInt(el.backlightTime.value, -1))); b.append("backlightCloseTime", String(toInt(el.backlightCloseTime.value, 20)));
    const r = await fetch("/api/effects", { method: "POST", body: b }); const raw = await r.text(); if (!r.ok) throw new Error(raw || `HTTP ${r.status}`); const d = JSON.parse(raw);
    await loadDisplayAudio();
    setStatus(el.displayStatus, `保存成功: 亮度=${d.backlight}, 息屏=${d.backlightTime}s, 关闭延时=${d.backlightCloseTime}s`);
  } catch (e) { setStatus(el.displayStatus, `保存失败: ${e.message}`, true); }
}
async function loadWireless() {
  try {
    const s = await fetchJsonSafeRetry("/api/status", "状态接口(/api/status)");
    const a = await fetchJsonSafeRetry("/api/apconfig", "AP配置接口(/api/apconfig)");
    const t = await fetchJsonSafeRetry("/api/staconfig", "联网配置接口(/api/staconfig)");
    const h = await fetchJsonSafeRetry("/api/hostmac", "HostMAC接口(/api/hostmac)");
    const active = document.activeElement;
    if (active !== el.apSsid) el.apSsid.value = a.ssid || "";
    if (active !== el.apPassword) el.apPassword.value = a.password || "";
    if (active !== el.apChannel) el.apChannel.value = String(a.channel || 1);
    if (active !== el.staSsid) el.staSsid.value = t.ssid || "";
    if (active !== el.staPassword) el.staPassword.value = t.password || "";
    if (active !== el.staNet) el.staNet.value = t.net || "";
    el.hostMac.value = h.hostMac || "";
    el.selfMac.value = s.selfMac || "";
    setStatus(el.wirelessStatus, `队列: Web ${s.queue} / Host ${s.hostQueue} | AP=${s.apSsid || "-"} CH=${s.apChannel || "-"}`);
  } catch (e) { setStatus(el.wirelessStatus, `加载无线信息失败: ${e.message}`, true); }
}

async function saveAp() {
  try {
    const b = new FormData(); b.append("ssid", el.apSsid.value.trim()); b.append("password", el.apPassword.value); b.append("channel", el.apChannel.value.trim());
    const r = await fetch("/api/apconfig", { method: "POST", body: b }); const raw = await r.text(); if (!r.ok) throw new Error(raw || `HTTP ${r.status}`); const d = JSON.parse(raw);
    await loadWireless();
    setStatus(el.wirelessStatus, `AP设置已保存: ${d.ssid} CH${d.channel}`);
  } catch (e) { setStatus(el.wirelessStatus, `AP设置保存失败: ${e.message}`, true); }
}

async function saveSta() {
  try {
    const b = new FormData(); b.append("ssid", el.staSsid.value.trim()); b.append("password", el.staPassword.value); b.append("net", el.staNet.value.trim());
    const r = await fetch("/api/staconfig", { method: "POST", body: b }); const raw = await r.text(); if (!r.ok) throw new Error(raw || `HTTP ${r.status}`); const d = JSON.parse(raw);
    await loadWireless();
    setStatus(el.wirelessStatus, `联网设置已保存: ${d.ssid} -> ${d.net || "-"}`);
  } catch (e) { setStatus(el.wirelessStatus, `联网设置保存失败: ${e.message}`, true); }
}

async function saveHostMac() {
  try {
    const b = new FormData(); b.append("hostMac", el.hostMac.value.trim());
    const r = await fetch("/api/hostmac", { method: "POST", body: b }); const raw = await r.text(); if (!r.ok) throw new Error(raw || `HTTP ${r.status}`); const d = JSON.parse(raw);
    await loadWireless();
    setStatus(el.wirelessStatus, d.enabled ? `HostMAC已设置: ${d.hostMac}` : "HostMAC过滤已关闭");
  } catch (e) { setStatus(el.wirelessStatus, `HostMAC保存失败: ${e.message}`, true); }
}

async function loadBattery() {
  try {
    const r = await fetch("/api/battery"); const d = await r.json(); if (!r.ok) throw new Error(`HTTP ${r.status}`);
    if (!d.ok) return setStatus(el.batteryStatus, "未接入电池检测硬件", true);
    setStatus(el.batteryStatus, `电量: ${d.percent}%`);
  } catch (e) { setStatus(el.batteryStatus, `读取失败: ${e.message}`, true); }
}

function bind() {
  el.tabMsg.addEventListener("click", () => switchPush("msg"));
  el.tabImg.addEventListener("click", () => switchPush("img"));
  el.msgHistory.addEventListener("change", () => {
    const idx = toInt(el.msgHistory.value, -1);
    if (idx < 0 || idx >= st.msgHistory.length) return;
    const picked = st.msgHistory[idx];
    el.msgInput.value = picked.msg || "";
    el.msgPrefix.value = picked.prefix || "";
    el.msgPrefixEnable.checked = !!(picked.prefix && picked.prefix.length);
    autoGrow(el.msgInput);
    saveMsgLocal();
  });
  el.msgPrefixEnable.addEventListener("change", saveMsgLocal); el.msgPrefix.addEventListener("input", saveMsgLocal);
  el.btnSendMsg.addEventListener("click", sendMsg); el.btnClearMsg.addEventListener("click", () => { el.msgInput.value = ""; autoGrow(el.msgInput); el.msgInput.focus(); });

  el.btnCopyPortalUrl.addEventListener("click", async () => setStatus(el.imgStatus, (await copyText(el.portalUrl.value || portalRoot())) ? "地址已复制" : "复制失败", false));
  el.imgFile.addEventListener("change", async (ev) => { st.imageFile = ev.target.files && ev.target.files[0] ? ev.target.files[0] : null; await prepImage(); });
  [el.imgOffsetY, el.imgFitMode, el.imgWidth, el.imgHeight].forEach((x) => { x.addEventListener("input", prepImage); x.addEventListener("change", prepImage); });
  el.btnSendImg.addEventListener("click", sendImage);
  el.btnClearImg.addEventListener("click", () => { st.imageFile = null; st.imageBuf = null; el.imgFile.value = ""; clearPreview(); setStatus(el.imgStatus, "已清除图片"); });

  el.btnCmdAdd.addEventListener("click", () => { const t = String(el.cmdAddInput.value || "").trim(); if (!t) return; st.commands.push(t); el.cmdAddInput.value = ""; renderCmdList(); setStatus(el.cmdStatus, "已添加（记得保存）"); });
  el.btnCmdSave.addEventListener("click", saveCommands);

  el.scheduleDate.addEventListener("change", renderSchedules); el.btnScheduleAdd.addEventListener("click", addSchedule); el.btnScheduleSave.addEventListener("click", saveSchedules);
  el.btnRtcSet.addEventListener("click", rtcSet); el.btnRtcSyncPhone.addEventListener("click", rtcSyncPhone);

  el.insertVol.addEventListener("input", () => { setVolUi(el.insertVol.value, el.bgVol.value); if (st.volTimer) clearTimeout(st.volTimer); st.volTimer = setTimeout(async () => { st.volTimer = null; try { const d = await pushVol(false); setVolUi(d.insertVolume, d.bgVolume); } catch (_) {} }, 120); });
  el.bgVol.addEventListener("input", () => { setVolUi(el.insertVol.value, el.bgVol.value); if (st.volTimer) clearTimeout(st.volTimer); st.volTimer = setTimeout(async () => { st.volTimer = null; try { const d = await pushVol(false); setVolUi(d.insertVolume, d.bgVolume); } catch (_) {} }, 120); });
  el.insertVol.addEventListener("change", async () => { try { const d = await pushVol(true); setVolUi(d.insertVolume, d.bgVolume); } catch (e) { setStatus(el.displayStatus, `音量保存失败: ${e.message}`, true); } });
  el.bgVol.addEventListener("change", async () => { try { const d = await pushVol(true); setVolUi(d.insertVolume, d.bgVolume); } catch (e) { setStatus(el.displayStatus, `音量保存失败: ${e.message}`, true); } });

  el.btnSaveDisplay.addEventListener("click", saveDisplay);
  el.btnSaveAp.addEventListener("click", saveAp);
  el.btnSaveSta.addEventListener("click", saveSta);
  el.btnSaveHostMac.addEventListener("click", saveHostMac);
  el.btnCopySelfMac.addEventListener("click", async () => {
    const t = String(el.selfMac.value || "").trim(); if (!t) return setStatus(el.wirelessStatus, "MAC为空", true);
    setStatus(el.wirelessStatus, (await copyText(t)) ? "MAC已复制" : "复制失败");
  });

  [el.msgInput, el.cmdAddInput, el.scheduleAddText].forEach((ta) => {
    if (!ta) return;
    autoGrow(ta);
    ta.addEventListener("input", () => autoGrow(ta));
  });
}

async function boot() {
  switchPush("msg"); bind(); loadMsgLocal();
  el.portalUrl.value = portalRoot();
  const n = new Date(); el.scheduleDate.value = `${n.getFullYear()}-${pad2(n.getMonth() + 1)}-${pad2(n.getDate())}`;
  clearPreview();
  await Promise.all([loadCommands(), loadSchedules(), loadRtc(), loadDisplayAudio(), loadWireless(), loadBattery()]);
  setInterval(loadRtc, 5000);
  setInterval(loadBattery, 5000);
}

boot();
