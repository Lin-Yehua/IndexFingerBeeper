#include "HostWirelessPortal.h"

#include <Arduino.h>
#include <FS.h>
#include <FFat.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_now.h>

#include "EspNowMessage.h"

namespace {

constexpr char kDefaultApSsid[] = "ESP-NOW-HOST";
constexpr char kDefaultApPassword[] = "12345678";
constexpr uint8_t kDefaultApChannel = 1;
constexpr uint8_t kApChannelMin = 1;
constexpr uint8_t kApChannelMax = 13;
constexpr size_t kApSsidMaxLen = 33;
constexpr size_t kApPasswordMaxLen = 65;
constexpr uint16_t kHttpPort = 80;
constexpr uint16_t kDnsPort = 53;
constexpr BaseType_t kWebTaskCore = 0;
constexpr UBaseType_t kWebTaskPriority = 1;
constexpr uint32_t kWebTaskStack = 8192;

const char kHostPortalHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP-NOW Host</title>
  <style>
    :root{--bg:#0b1017;--card:#182435;--edge:#2e4661;--text:#f4f8ff;--muted:#b4c5dd;--accent:#53e4ff;--accent2:#8eff7a;--danger:#ff8f8f}
    *{box-sizing:border-box}
    body{margin:0;font-family:"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;background:radial-gradient(120% 120% at 50% 0%,#102138 0%,var(--bg) 60%);color:var(--text)}
    .page{width:min(900px,100%);margin:0 auto;padding:16px}
    .hero{margin-bottom:14px}
    .hero h1{margin:0 0 8px;font-size:28px;line-height:1.15;color:var(--accent)}
    .hero p{margin:0;color:var(--muted)}
    .card{border:1px solid var(--edge);background:linear-gradient(180deg,#1b2d43,var(--card));border-radius:14px;padding:14px;margin-bottom:14px}
    .card h2{margin:0 0 10px;font-size:18px}
    textarea,input{width:100%;padding:12px;border-radius:10px;border:1px solid #456181;background:#0d1825;color:var(--text)}
    textarea{min-height:130px;resize:vertical}
    .row{display:flex;gap:10px;margin-top:10px;flex-wrap:wrap}
    button{border:none;border-radius:10px;padding:10px 14px;min-width:110px;font-weight:600;color:#04111a;background:linear-gradient(135deg,var(--accent),var(--accent2));cursor:pointer}
    button.ghost{color:var(--text);background:#2b415c}
    .status{min-height:20px;margin:10px 0 0;color:var(--muted);white-space:pre-wrap}
    .status.error{color:var(--danger)}
    .tip{font-size:13px;color:var(--muted)}
  </style>
</head>
<body>
  <main class="page">
    <header class="hero">
      <h1>ESP-NOW Host</h1>
      <p>Broadcast message to all clients. Config can be saved to <code>/setting.ini</code>.</p>
    </header>

    <section class="card">
      <h2>1) Broadcast Message</h2>
      <textarea id="msgBox" maxlength="192" placeholder="Text to broadcast"></textarea>
      <div class="row">
        <button id="btnSend" type="button">Broadcast</button>
        <button id="btnClear" type="button" class="ghost">Clear</button>
      </div>
      <p id="sendStatus" class="status"></p>
    </section>

    <section class="card">
      <h2>2) Host Communication Config</h2>
      <input id="ssidInput" type="text" maxlength="32" placeholder="SSID">
      <div class="row"></div>
      <input id="passwordInput" type="text" maxlength="63" placeholder="Password (empty = open AP)">
      <div class="row"></div>
      <input id="channelInput" type="number" min="1" max="13" step="1" placeholder="ESP-NOW / AP channel (1-13)">
      <div class="row">
        <button id="btnSaveConfig" type="button">Save Config</button>
      </div>
      <p id="configStatus" class="status"></p>
      <p class="tip">Save writes to /setting.ini. Reboot to apply new AP config.</p>
    </section>

    <section class="card">
      <h2>3) Runtime Status</h2>
      <p id="runtimeStatus" class="status"></p>
    </section>
  </main>

  <script>
    const msgBox = document.getElementById('msgBox');
    const sendStatus = document.getElementById('sendStatus');
    const runtimeStatus = document.getElementById('runtimeStatus');
    const ssidInput = document.getElementById('ssidInput');
    const passwordInput = document.getElementById('passwordInput');
    const channelInput = document.getElementById('channelInput');
    const configStatus = document.getElementById('configStatus');

    function setStatus(el, text, isError = false) {
      el.textContent = text || '';
      el.classList.toggle('error', !!isError);
    }

    async function sendBroadcast() {
      try {
        const body = new FormData();
        body.append('text', msgBox.value);
        const resp = await fetch('/api/send', { method: 'POST', body });
        const text = await resp.text();
        if (!resp.ok) throw new Error(text || ('HTTP ' + resp.status));
        setStatus(sendStatus, text || 'broadcast sent');
        msgBox.value = '';
      } catch (err) {
        setStatus(sendStatus, 'Broadcast failed: ' + err.message, true);
      }
    }

    async function loadConfig() {
      try {
        const resp = await fetch('/api/config');
        const data = await resp.json();
        if (!resp.ok) throw new Error('HTTP ' + resp.status);
        ssidInput.value = data.ssid || '';
        passwordInput.value = '';
        channelInput.value = String(data.channel || 1);
        setStatus(configStatus, data.passwordSet ? 'Password is set on device' : 'Open AP (no password)');
      } catch (err) {
        setStatus(configStatus, 'Config load failed: ' + err.message, true);
      }
    }

    async function saveConfig() {
      try {
        const body = new FormData();
        body.append('ssid', ssidInput.value.trim());
        body.append('password', passwordInput.value);
        body.append('channel', channelInput.value.trim());
        const resp = await fetch('/api/config', { method: 'POST', body });
        const raw = await resp.text();
        if (!resp.ok) throw new Error(raw || ('HTTP ' + resp.status));
        const data = JSON.parse(raw);
        setStatus(configStatus, 'Saved: SSID=' + data.ssid + ', CH=' + data.channel + '. Reboot required to fully apply.');
        passwordInput.value = '';
      } catch (err) {
        setStatus(configStatus, 'Config save failed: ' + err.message, true);
      }
    }

    async function refreshStatus() {
      try {
        const resp = await fetch('/api/status');
        const data = await resp.json();
        if (!resp.ok) throw new Error('HTTP ' + resp.status);
        setStatus(runtimeStatus, 'SSID: ' + data.ssid + ' | IP: ' + data.ip + ' | CH: ' + data.channel + ' | MAC: ' + data.mac);
      } catch (err) {
        setStatus(runtimeStatus, 'Status failed: ' + err.message, true);
      }
    }

    document.getElementById('btnSend').addEventListener('click', sendBroadcast);
    document.getElementById('btnClear').addEventListener('click', () => {
      msgBox.value = '';
      msgBox.focus();
    });
    document.getElementById('btnSaveConfig').addEventListener('click', saveConfig);

    loadConfig();
    refreshStatus();
    setInterval(refreshStatus, 1000);
  </script>
</body>
</html>
)HTML";

TaskHandle_t gWebTaskHandle = nullptr;
WebServer *gWebServer = nullptr;
DNSServer *gDnsServer = nullptr;
volatile bool gWebTaskRunning = false;
bool gPortalStarted = false;
bool gEspNowReady = false;
uint16_t gEspNowSeq = 0;
char gApSsid[kApSsidMaxLen] = {0};
char gApPassword[kApPasswordMaxLen] = {0};
uint8_t gApChannel = kDefaultApChannel;

void copyStringToBuf(const String &src, char *dst, size_t dstSize) {
  if (!dst || dstSize == 0) return;
  if (!src.length()) {
    dst[0] = '\0';
    return;
  }
  src.toCharArray(dst, dstSize);
  dst[dstSize - 1] = '\0';
}

uint8_t clampApChannel(int channel) {
  if (channel < static_cast<int>(kApChannelMin)) return kApChannelMin;
  if (channel > static_cast<int>(kApChannelMax)) return kApChannelMax;
  return static_cast<uint8_t>(channel);
}

bool parseApChannel(const String &raw, uint8_t &outChannel) {
  String s = raw;
  s.trim();
  if (!s.length()) return false;
  const int channel = s.toInt();
  if (channel < static_cast<int>(kApChannelMin) || channel > static_cast<int>(kApChannelMax)) {
    return false;
  }
  outChannel = static_cast<uint8_t>(channel);
  return true;
}

String stripIniValue(String value) {
  value.trim();
  const int semicolon = value.indexOf(';');
  if (semicolon >= 0) value = value.substring(0, semicolon);
  const int hash = value.indexOf('#');
  if (hash >= 0) value = value.substring(0, hash);
  value.trim();
  if (value.length() >= 2) {
    const char first = value[0];
    const char last = value[value.length() - 1];
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      value = value.substring(1, value.length() - 1);
      value.trim();
    }
  }
  return value;
}

void loadApCredentialsFromSettingIni() {
  copyStringToBuf(String(kDefaultApSsid), gApSsid, sizeof(gApSsid));
  copyStringToBuf(String(kDefaultApPassword), gApPassword, sizeof(gApPassword));
  gApChannel = kDefaultApChannel;

  fs::File f = FFat.open("/setting.ini", FILE_READ);
  if (!f) {
    Serial.println("[HOST] /setting.ini not found, using default AP config");
    return;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    if (line.startsWith("#") || line.startsWith(";")) continue;

    const int eq = line.indexOf('=');
    if (eq <= 0) continue;

    String key = line.substring(0, eq);
    String value = line.substring(eq + 1);
    key.trim();
    key.toLowerCase();
    value = stripIniValue(value);

    if (key == "ssip" || key == "ssid") {
      if (value.length()) copyStringToBuf(value, gApSsid, sizeof(gApSsid));
    } else if (key == "password") {
      if (!value.length()) {
        gApPassword[0] = '\0';
      } else if (value.length() >= 8) {
        copyStringToBuf(value, gApPassword, sizeof(gApPassword));
      } else {
        gApPassword[0] = '\0';
      }
    } else if (key == "espnowchannel" || key == "channel" || key == "apchannel") {
      uint8_t parsed = kDefaultApChannel;
      if (parseApChannel(value, parsed)) {
        gApChannel = parsed;
      } else {
        gApChannel = kDefaultApChannel;
      }
    }
  }
  f.close();
}

bool persistHostConfigToSettingIni(const String &ssidRaw, const String &passwordRaw, uint8_t channel) {
  String ssid = ssidRaw;
  String password = passwordRaw;
  ssid.trim();
  password.trim();
  channel = clampApChannel(channel);

  if (!ssid.length()) return false;
  if (ssid.length() >= sizeof(gApSsid)) return false;
  if (password.length() && (password.length() < 8 || password.length() > 63)) return false;

  String original;
  if (FFat.exists("/setting.ini")) {
    fs::File rf = FFat.open("/setting.ini", FILE_READ);
    if (!rf) return false;
    original = rf.readString();
    rf.close();
  }

  bool foundSsid = false;
  bool foundPassword = false;
  bool foundChannel = false;
  String output;
  output.reserve(original.length() + 128);

  int start = 0;
  while (start <= original.length()) {
    const int end = original.indexOf('\n', start);
    String line = (end >= 0) ? original.substring(start, end) : original.substring(start);

    String trimmed = line;
    trimmed.trim();
    if (trimmed.length() && !trimmed.startsWith("#") && !trimmed.startsWith(";")) {
      const int eq = trimmed.indexOf('=');
      if (eq > 0) {
        String key = trimmed.substring(0, eq);
        key.trim();
        key.toLowerCase();
        if (key == "ssip" || key == "ssid") {
          line = "SSID = \"" + ssid + "\";";
          foundSsid = true;
        } else if (key == "password") {
          line = "Password = \"" + password + "\";";
          foundPassword = true;
        } else if (key == "espnowchannel" || key == "channel" || key == "apchannel") {
          line = "EspNowChannel = " + String(static_cast<unsigned int>(channel)) + ";";
          foundChannel = true;
        }
      }
    }

    output += line;
    if (end >= 0) {
      output += '\n';
      start = end + 1;
    } else {
      break;
    }
  }

  if (!foundSsid) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "SSID = \"" + ssid + "\";\n";
  }
  if (!foundPassword) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "Password = \"" + password + "\";\n";
  }
  if (!foundChannel) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "EspNowChannel = " + String(static_cast<unsigned int>(channel)) + ";\n";
  }

  fs::File wf = FFat.open("/setting.ini", "w");
  if (!wf) return false;
  const size_t written = wf.print(output);
  wf.close();
  return written == output.length();
}

String apRootUrl() {
  String url = "http://";
  url += WiFi.softAPIP().toString();
  url += "/";
  return url;
}

void redirectToPortal() {
  gWebServer->sendHeader("Location", apRootUrl(), true);
  gWebServer->send(302, "text/plain", "");
}

bool initEspNowBroadcaster() {
  if (gEspNowReady) return true;
  if (esp_now_init() != ESP_OK) {
    Serial.println("[HOST][ESPNOW] init failed");
    return false;
  }

  uint8_t broadcastAddr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddr, sizeof(broadcastAddr));
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_AP;
  const esp_err_t addRet = esp_now_add_peer(&peer);
  if (addRet != ESP_OK && addRet != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf("[HOST][ESPNOW] add peer failed: %d\n", static_cast<int>(addRet));
    esp_now_deinit();
    return false;
  }

  gEspNowReady = true;
  Serial.println("[HOST][ESPNOW] broadcaster ready");
  return true;
}

void deinitEspNowBroadcaster() {
  if (!gEspNowReady) return;
  esp_now_deinit();
  gEspNowReady = false;
}

bool sendBroadcastText(const String &rawText, String &errorOut) {
  if (!gEspNowReady) {
    errorOut = "esp-now not ready";
    return false;
  }

  String text = rawText;
  text.trim();
  if (!text.length()) {
    errorOut = "text is empty";
    return false;
  }
  if (text.length() > kEspNowTextMaxBytes - 1) {
    text.remove(kEspNowTextMaxBytes - 1);
  }

  EspNowTextPacket pkt = {};
  pkt.magic = kEspNowTextMagic;
  pkt.type = kEspNowMsgTypeText;
  pkt.seq = gEspNowSeq++;
  text.toCharArray(pkt.text, sizeof(pkt.text));

  uint8_t broadcastAddr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  const esp_err_t ret = esp_now_send(broadcastAddr, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt));
  if (ret != ESP_OK) {
    errorOut = "esp-now send failed";
    return false;
  }
  return true;
}

String statusJson() {
  String out = "{\"ip\":\"";
  out += WiFi.softAPIP().toString();
  out += "\",\"ssid\":\"";
  out += String(gApSsid);
  out += "\",\"mac\":\"";
  out += WiFi.softAPmacAddress();
  out += "\",\"channel\":";
  out += String(WiFi.channel());
  out += ",\"passwordSet\":";
  out += gApPassword[0] ? "true" : "false";
  out += "}";
  return out;
}

void sendPortalPage() {
  gWebServer->send_P(200, "text/html; charset=utf-8", kHostPortalHtml);
}

void registerRoutes() {
  gWebServer->on("/", HTTP_GET, []() {
    sendPortalPage();
  });
  gWebServer->on("/index.html", HTTP_GET, []() {
    sendPortalPage();
  });

  gWebServer->on("/api/status", HTTP_GET, []() {
    gWebServer->send(200, "application/json", statusJson());
  });

  gWebServer->on("/api/send", HTTP_POST, []() {
    String text = gWebServer->arg("text");
    if (!text.length() && gWebServer->hasArg("plain")) {
      text = gWebServer->arg("plain");
    }

    String error;
    if (!sendBroadcastText(text, error)) {
      gWebServer->send(400, "text/plain", error);
      return;
    }

    gWebServer->send(200, "text/plain", "broadcast sent");
  });

  gWebServer->on("/api/config", HTTP_GET, []() {
    String out = "{\"ssid\":\"";
    out += String(gApSsid);
    out += "\",\"passwordSet\":";
    out += gApPassword[0] ? "true" : "false";
    out += ",\"channel\":";
    out += String(static_cast<unsigned int>(gApChannel));
    out += "}";
    gWebServer->send(200, "application/json", out);
  });

  gWebServer->on("/api/config", HTTP_POST, []() {
    String ssid = gWebServer->arg("ssid");
    if (!ssid.length() && gWebServer->hasArg("ssip")) {
      ssid = gWebServer->arg("ssip");
    }
    if (!ssid.length() && gWebServer->hasArg("plain")) {
      ssid = gWebServer->arg("plain");
    }
    String password = gWebServer->arg("password");
    String channelArg = gWebServer->arg("channel");

    ssid.trim();
    password.trim();
    channelArg.trim();

    if (!ssid.length()) {
      gWebServer->send(400, "text/plain", "ssid is empty");
      return;
    }
    if (ssid.length() >= sizeof(gApSsid)) {
      gWebServer->send(400, "text/plain", "ssid too long");
      return;
    }
    if (password.length() && (password.length() < 8 || password.length() > 63)) {
      gWebServer->send(400, "text/plain", "password must be empty or 8-63 chars");
      return;
    }

    uint8_t nextChannel = gApChannel;
    if (channelArg.length() && !parseApChannel(channelArg, nextChannel)) {
      gWebServer->send(400, "text/plain", "channel must be 1-13");
      return;
    }

    if (!persistHostConfigToSettingIni(ssid, password, nextChannel)) {
      gWebServer->send(500, "text/plain", "save /setting.ini failed");
      return;
    }

    copyStringToBuf(ssid, gApSsid, sizeof(gApSsid));
    if (!password.length()) {
      gApPassword[0] = '\0';
    } else {
      copyStringToBuf(password, gApPassword, sizeof(gApPassword));
    }
    gApChannel = nextChannel;

    String out = "{\"ssid\":\"";
    out += String(gApSsid);
    out += "\",\"passwordSet\":";
    out += gApPassword[0] ? "true" : "false";
    out += ",\"channel\":";
    out += String(static_cast<unsigned int>(gApChannel));
    out += ",\"rebootRequired\":true}";
    gWebServer->send(200, "application/json", out);
  });

  auto captiveRedirect = []() {
    redirectToPortal();
  };
  gWebServer->on("/generate_204", HTTP_GET, captiveRedirect);
  gWebServer->on("/gen_204", HTTP_GET, captiveRedirect);
  gWebServer->on("/hotspot-detect.html", HTTP_GET, captiveRedirect);
  gWebServer->on("/connecttest.txt", HTTP_GET, captiveRedirect);
  gWebServer->on("/ncsi.txt", HTTP_GET, captiveRedirect);
  gWebServer->on("/fwlink", HTTP_GET, captiveRedirect);

  gWebServer->onNotFound([]() {
    const String uri = gWebServer->uri();
    if (uri.startsWith("/api/")) {
      gWebServer->send(404, "text/plain", "not found");
      return;
    }
    redirectToPortal();
  });
}

void webServerTask(void *param) {
  (void)param;
  gWebServer = new WebServer(kHttpPort);
  gDnsServer = new DNSServer();
  if (!gWebServer || !gDnsServer) {
    if (gDnsServer) {
      delete gDnsServer;
      gDnsServer = nullptr;
    }
    if (gWebServer) {
      delete gWebServer;
      gWebServer = nullptr;
    }
    gWebTaskRunning = false;
    gWebTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  gDnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  gDnsServer->start(kDnsPort, "*", WiFi.softAPIP());

  registerRoutes();
  gWebServer->begin();
  Serial.printf("[HOST][WEB] HTTP ready: %s\n", apRootUrl().c_str());

  while (gWebTaskRunning) {
    gDnsServer->processNextRequest();
    gWebServer->handleClient();
    vTaskDelay(pdMS_TO_TICKS(8));
  }

  gWebServer->stop();
  gDnsServer->stop();
  delete gWebServer;
  delete gDnsServer;
  gWebServer = nullptr;
  gDnsServer = nullptr;
  gWebTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

bool hostPortalStart() {
  if (gPortalStarted) return true;

  loadApCredentialsFromSettingIni();

  WiFi.mode(WIFI_AP_STA);
  bool apOk = false;
  if (gApPassword[0] == '\0') {
    apOk = WiFi.softAP(gApSsid, nullptr, gApChannel);
  } else {
    apOk = WiFi.softAP(gApSsid, gApPassword, gApChannel);
  }
  if (!apOk) {
    Serial.println("[HOST][WEB] softAP start failed");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  if (!initEspNowBroadcaster()) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  gWebTaskRunning = true;
  const BaseType_t ok = xTaskCreatePinnedToCore(
      webServerTask,
      "HostWebPortal",
      kWebTaskStack,
      nullptr,
      kWebTaskPriority,
      &gWebTaskHandle,
      kWebTaskCore);

  if (ok != pdPASS) {
    gWebTaskRunning = false;
    deinitEspNowBroadcaster();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[HOST][WEB] task create failed");
    return false;
  }

  gPortalStarted = true;
  Serial.printf("[HOST] AP started SSID=%s PASS=%s IP=%s CH=%d MAC=%s\n",
                gApSsid,
                gApPassword[0] ? gApPassword : "<OPEN>",
                WiFi.softAPIP().toString().c_str(),
                WiFi.channel(),
                WiFi.softAPmacAddress().c_str());
  return true;
}

void hostPortalStop() {
  if (!gPortalStarted) return;

  gWebTaskRunning = false;
  for (int i = 0; i < 120 && gWebTaskHandle != nullptr; ++i) {
    delay(5);
  }
  if (gWebTaskHandle != nullptr) {
    vTaskDelete(gWebTaskHandle);
    gWebTaskHandle = nullptr;
  }
  if (gWebServer) {
    gWebServer->stop();
    delete gWebServer;
    gWebServer = nullptr;
  }
  if (gDnsServer) {
    gDnsServer->stop();
    delete gDnsServer;
    gDnsServer = nullptr;
  }

  deinitEspNowBroadcaster();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  gPortalStarted = false;
}
