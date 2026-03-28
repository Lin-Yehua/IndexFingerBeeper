#include "WirelessPortal.h"

#include <Arduino.h>
#include <FS.h>
#include <FFat.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

#include "AppGlobals.h"

namespace {

constexpr char kApSsid[] = u8"\u9075\u4ECE\u90FD\u5E02\u610F\u5FD7";
constexpr char kApPassword[] = "12345678";
constexpr uint16_t kHttpPort = 80;
constexpr uint16_t kDnsPort = 53;
constexpr BaseType_t kWebTaskCore = 0;
constexpr UBaseType_t kWebTaskPriority = 1;
constexpr uint32_t kWebTaskStack = 8192;
constexpr size_t kTextMaxLen = 240;
constexpr size_t kQueueDepth = 128;

struct WebQueuedMessage {
  char text[kTextMaxLen + 1];
};

QueueHandle_t gMessageQueue = nullptr;
TaskHandle_t gWebTaskHandle = nullptr;
WebServer *gWebServer = nullptr;
DNSServer *gDnsServer = nullptr;
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

bool enqueueMessage(const String &textRaw, String &errorOut) {
  if (!gMessageQueue) {
    errorOut = "queue not ready";
    return false;
  }

  String text = textRaw;
  text.trim();
  if (!text.length()) {
    errorOut = "text is empty";
    return false;
  }
  if (text.length() > kTextMaxLen) text.remove(kTextMaxLen);

  WebQueuedMessage msg = {};
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

String detectContentType(const String &path) {
  if (path.endsWith(".html")) return "text/html; charset=utf-8";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".txt")) return "text/plain; charset=utf-8";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".svg")) return "image/svg+xml";
  return "application/octet-stream";
}

bool streamFromLittleFs(const String &path) {
  if (!LittleFS.exists(path)) return false;
  fs::File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  gWebServer->sendHeader("Cache-Control", "no-store");
  gWebServer->streamFile(f, detectContentType(path));
  f.close();
  return true;
}

void handleStaticFile(const String &path) {
  if (!streamFromLittleFs(path)) {
    gWebServer->send(404, "text/plain", "file not found");
  }
}

void registerRoutes() {
  gWebServer->on("/", HTTP_GET, []() {
    if (!streamFromLittleFs("/index.html")) {
      gWebServer->send(500, "text/plain", "missing /index.html in LittleFS");
    }
  });

  gWebServer->on("/index.html", HTTP_GET, []() {
    handleStaticFile("/index.html");
  });
  gWebServer->on("/style.css", HTTP_GET, []() {
    handleStaticFile("/style.css");
  });
  gWebServer->on("/app.js", HTTP_GET, []() {
    handleStaticFile("/app.js");
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
    String text = gWebServer->arg("text");
    if (!text.length() && gWebServer->hasArg("plain")) {
      text = gWebServer->arg("plain");
    }
    if (!enqueueMessage(text, error)) {
      const int code = (error == "queue is full") ? 503 : 400;
      gWebServer->send(code, "text/plain", error);
      return;
    }
    const UBaseType_t queued = uxQueueMessagesWaiting(gMessageQueue);
    String reply = "queued, waiting=";
    reply += String(static_cast<unsigned int>(queued));
    gWebServer->send(200, "text/plain", reply);
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

    if (LittleFS.exists(uri)) {
      handleStaticFile(uri);
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
  Serial.printf("[WEB] HTTP ready: %s\n", apRootUrl().c_str());

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

bool wirelessPortalStart() {
  if (gPortalStarted) return true;

  if (!LittleFS.exists("/index.html") ||
      !LittleFS.exists("/style.css") ||
      !LittleFS.exists("/app.js")) {
    Serial.println("[WEB] missing web files in LittleFS (/index.html /style.css /app.js)");
    Serial.println("[WEB] run: pio run -t uploadfs -e 4d_systems_esp32s3_gen4_r8n16");
    return false;
  }

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

  if (gDnsServer) {
    gDnsServer->stop();
    delete gDnsServer;
    gDnsServer = nullptr;
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

  outMessage = msg.text;
  return outMessage.length() > 0;
}

bool wirelessPortalHasPendingMessage() {
  if (!gMessageQueue) return false;
  return uxQueueMessagesWaiting(gMessageQueue) > 0;
}

bool wirelessPortalConsumeCsvReloadRequest() {
  return takeCsvReloadRequested();
}

