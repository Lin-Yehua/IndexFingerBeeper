#include "WirelessPortal.h"

#include <Arduino.h>
#include <FS.h>
#include <FFat.h>
#include <WiFi.h>
#include <WebServer.h>

#include "AppGlobals.h"

namespace {

constexpr char kApSsid[] = "ESP32-TFT-AP";
constexpr char kApPassword[] = "12345678";
constexpr uint16_t kHttpPort = 80;
constexpr BaseType_t kWebTaskCore = 0;
constexpr UBaseType_t kWebTaskPriority = 1;
constexpr uint32_t kWebTaskStack = 8192;
constexpr size_t kSenderMaxLen = 24;
constexpr size_t kTextMaxLen = 240;
constexpr size_t kQueueDepth = 24;

struct WebQueuedMessage {
  char sender[kSenderMaxLen + 1];
  char text[kTextMaxLen + 1];
};

QueueHandle_t gMessageQueue = nullptr;
TaskHandle_t gWebTaskHandle = nullptr;
WebServer *gWebServer = nullptr;
volatile bool gWebTaskRunning = false;
volatile bool gCsvReloadRequested = false;
bool gPortalStarted = false;
portMUX_TYPE gFlagMux = portMUX_INITIALIZER_UNLOCKED;

void setCsvReloadRequested() {
  portENTER_CRITICAL(&gFlagMux);
  gCsvReloadRequested = true;
  portEXIT_CRITICAL(&gFlagMux);
}

bool takeCsvReloadRequested() {
  bool value = false;
  portENTER_CRITICAL(&gFlagMux);
  value = gCsvReloadRequested;
  gCsvReloadRequested = false;
  portEXIT_CRITICAL(&gFlagMux);
  return value;
}

String readDataCsvText() {
  if (!fatMounted) return "";
  fs::File f = FFat.open("/data.csv", FILE_READ);
  if (!f) return "";
  String content = f.readString();
  f.close();
  return content;
}

bool saveDataCsvText(const String &content) {
  if (!fatMounted) return false;
  fs::File f = FFat.open("/data.csv", "w");
  if (!f) return false;
  const size_t written = f.print(content);
  f.close();
  if (written != content.length()) return false;
  setCsvReloadRequested();
  return true;
}

bool enqueueMessage(const String &senderRaw, const String &textRaw, String &errorOut) {
  if (!gMessageQueue) {
    errorOut = "queue not ready";
    return false;
  }

  String sender = senderRaw;
  sender.trim();
  if (!sender.length()) sender = "WEB";
  if (sender.length() > kSenderMaxLen) sender.remove(kSenderMaxLen);

  String text = textRaw;
  text.trim();
  if (!text.length()) {
    errorOut = "text is empty";
    return false;
  }
  if (text.length() > kTextMaxLen) text.remove(kTextMaxLen);

  WebQueuedMessage msg = {};
  sender.toCharArray(msg.sender, sizeof(msg.sender));
  text.toCharArray(msg.text, sizeof(msg.text));

  if (xQueueSend(gMessageQueue, &msg, 0) != pdTRUE) {
    errorOut = "queue is full";
    return false;
  }
  return true;
}

String statusJson() {
  const UBaseType_t queued = gMessageQueue ? uxQueueMessagesWaiting(gMessageQueue) : 0;
  bool pendingReload = false;
  portENTER_CRITICAL(&gFlagMux);
  pendingReload = gCsvReloadRequested;
  portEXIT_CRITICAL(&gFlagMux);

  String out = "{\"queue\":";
  out += String(static_cast<unsigned int>(queued));
  out += ",\"csvReloadPending\":";
  out += pendingReload ? "true" : "false";
  out += "}";
  return out;
}

String buildIndexHtml() {
  String html;
  html.reserve(5200);
  html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>ESP32 AP Control</title><style>";
  html += "body{font-family:Arial,sans-serif;background:#101820;color:#f2f2f2;margin:0;padding:14px;}";
  html += "h1{margin:0 0 8px;font-size:22px;}h2{font-size:18px;margin:8px 0;}";
  html += ".card{background:#1d2a3a;border-radius:10px;padding:12px;margin-bottom:12px;}";
  html += "textarea,input{width:100%;border:0;border-radius:8px;padding:10px;box-sizing:border-box;}";
  html += "textarea{min-height:180px;resize:vertical;}button{margin-top:8px;border:0;border-radius:8px;padding:10px 14px;cursor:pointer;}";
  html += ".row{display:flex;gap:8px;flex-wrap:wrap;}.row button{flex:1;min-width:120px;}";
  html += ".status{margin-top:8px;font-size:13px;opacity:0.85;white-space:pre-wrap;}";
  html += "</style></head><body>";
  html += "<h1>ESP32 AP Control</h1>";
  html += "<div class='card'><h2>1) Edit /data.csv</h2>";
  html += "<div class='row'><button onclick='loadCsv()'>Load CSV</button><button onclick='saveCsv()'>Save CSV</button></div>";
  html += "<textarea id='csvBox' placeholder='id,text'></textarea><div id='csvStatus' class='status'></div></div>";
  html += "<div class='card'><h2>2) Push Message Queue</h2>";
  html += "<input id='sender' maxlength='24' placeholder='Sender (optional)'>";
  html += "<textarea id='msg' maxlength='240' placeholder='Message to show on screen'></textarea>";
  html += "<div class='row'><button onclick='sendMsg()'>Send</button><button onclick='clearMsg()'>Clear</button></div>";
  html += "<div id='msgStatus' class='status'></div></div>";
  html += "<script>";
  html += "const csvBox=document.getElementById('csvBox');";
  html += "const csvStatus=document.getElementById('csvStatus');";
  html += "const msgStatus=document.getElementById('msgStatus');";
  html += "async function loadCsv(){try{const r=await fetch('/api/csv');const t=await r.text();if(!r.ok)throw new Error(t||('HTTP '+r.status));csvBox.value=t;csvStatus.textContent='CSV loaded';}catch(e){csvStatus.textContent='Load failed: '+e.message;}}";
  html += "async function saveCsv(){try{const fd=new FormData();fd.append('content',csvBox.value);const r=await fetch('/api/csv',{method:'POST',body:fd});const t=await r.text();if(!r.ok)throw new Error(t||('HTTP '+r.status));csvStatus.textContent=t||'CSV saved';}catch(e){csvStatus.textContent='Save failed: '+e.message;}}";
  html += "async function sendMsg(){try{const fd=new FormData();fd.append('sender',document.getElementById('sender').value);fd.append('text',document.getElementById('msg').value);const r=await fetch('/api/send',{method:'POST',body:fd});const t=await r.text();if(!r.ok)throw new Error(t||('HTTP '+r.status));msgStatus.textContent=t||'Queued';document.getElementById('msg').value='';await refreshStatus();}catch(e){msgStatus.textContent='Send failed: '+e.message;}}";
  html += "function clearMsg(){document.getElementById('msg').value='';}";
  html += "async function refreshStatus(){try{const r=await fetch('/api/status');const j=await r.json();msgStatus.textContent='Queue: '+j.queue+' | csvReloadPending: '+j.csvReloadPending;}catch(e){}}";
  html += "setInterval(refreshStatus,1000);loadCsv();refreshStatus();";
  html += "</script></body></html>";
  return html;
}

void registerRoutes() {
  gWebServer->on("/", HTTP_GET, []() {
    gWebServer->send(200, "text/html", buildIndexHtml());
  });

  gWebServer->on("/api/status", HTTP_GET, []() {
    gWebServer->send(200, "application/json", statusJson());
  });

  gWebServer->on("/api/csv", HTTP_GET, []() {
    if (!fatMounted) {
      gWebServer->send(503, "text/plain", "FAT not mounted");
      return;
    }
    const String csvText = readDataCsvText();
    gWebServer->send(200, "text/plain", csvText);
  });

  gWebServer->on("/api/csv", HTTP_POST, []() {
    String content = gWebServer->arg("content");
    if (!content.length() && gWebServer->hasArg("plain")) {
      content = gWebServer->arg("plain");
    }
    if (!content.length()) {
      gWebServer->send(400, "text/plain", "content is empty");
      return;
    }
    if (!saveDataCsvText(content)) {
      gWebServer->send(500, "text/plain", "save failed");
      return;
    }
    gWebServer->send(200, "text/plain", "saved /data.csv, reload scheduled");
  });

  gWebServer->on("/api/send", HTTP_POST, []() {
    String error;
    const String sender = gWebServer->arg("sender");
    String text = gWebServer->arg("text");
    if (!text.length() && gWebServer->hasArg("plain")) {
      text = gWebServer->arg("plain");
    }
    if (!enqueueMessage(sender, text, error)) {
      const int code = (error == "queue is full") ? 503 : 400;
      gWebServer->send(code, "text/plain", error);
      return;
    }
    const UBaseType_t queued = uxQueueMessagesWaiting(gMessageQueue);
    String reply = "queued, waiting=";
    reply += String(static_cast<unsigned int>(queued));
    gWebServer->send(200, "text/plain", reply);
  });

  gWebServer->onNotFound([]() {
    gWebServer->send(404, "text/plain", "not found");
  });
}

void webServerTask(void *param) {
  (void)param;
  gWebServer = new WebServer(kHttpPort);
  if (!gWebServer) {
    gWebTaskRunning = false;
    gWebTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  registerRoutes();
  gWebServer->begin();
  Serial.printf("[WEB] HTTP ready: http://%s/\n", WiFi.softAPIP().toString().c_str());

  while (gWebTaskRunning) {
    gWebServer->handleClient();
    vTaskDelay(pdMS_TO_TICKS(8));
  }

  gWebServer->stop();
  delete gWebServer;
  gWebServer = nullptr;
  gWebTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

bool wirelessPortalStart() {
  if (gPortalStarted) return true;

  if (!gMessageQueue) {
    gMessageQueue = xQueueCreate(kQueueDepth, sizeof(WebQueuedMessage));
    if (!gMessageQueue) {
      Serial.println("[WEB] queue create failed");
      return false;
    }
  }

  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(kApSsid, kApPassword)) {
    Serial.println("[WEB] softAP start failed");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  gWebTaskRunning = true;
  const BaseType_t ok = xTaskCreatePinnedToCore(
      webServerTask,
      "WebPortal",
      kWebTaskStack,
      nullptr,
      kWebTaskPriority,
      &gWebTaskHandle,
      kWebTaskCore);

  if (ok != pdPASS) {
    gWebTaskRunning = false;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[WEB] task create failed");
    return false;
  }

  gPortalStarted = true;
  Serial.printf("[WEB] AP started SSID=%s PASS=%s IP=%s\n",
                kApSsid, kApPassword, WiFi.softAPIP().toString().c_str());
  return true;
}

void wirelessPortalStop() {
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

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  if (gMessageQueue) {
    WebQueuedMessage drop = {};
    while (xQueueReceive(gMessageQueue, &drop, 0) == pdTRUE) {}
  }

  portENTER_CRITICAL(&gFlagMux);
  gCsvReloadRequested = false;
  portEXIT_CRITICAL(&gFlagMux);

  gPortalStarted = false;
  Serial.println("[WEB] AP stopped");
}

bool wirelessPortalPopMessage(String &outMessage) {
  outMessage = "";
  if (!gMessageQueue) return false;

  WebQueuedMessage msg = {};
  if (xQueueReceive(gMessageQueue, &msg, 0) != pdTRUE) return false;

  outMessage.reserve(strlen(msg.sender) + strlen(msg.text) + 4);
  outMessage += "[";
  outMessage += msg.sender;
  outMessage += "] ";
  outMessage += msg.text;
  return true;
}

bool wirelessPortalConsumeCsvReloadRequest() {
  return takeCsvReloadRequested();
}
