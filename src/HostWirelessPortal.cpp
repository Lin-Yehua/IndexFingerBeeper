/*
 * 文件说明: 模块实现文件。
 * 文件功能: 实现对应模块的运行逻辑和内部辅助函数。
 *
 * 函数表:
 * - copyStringToBuf: 模块内部辅助函数。
 * - clampApChannel: 计算、判断或转换对应结果。
 * - parseApChannel: 解析、规范化或格式化对应内容。
 * - parseBoolString: 解析、规范化或格式化对应内容。
 * - forceWifiChannel: 应用配置或切换运行状态。
 * - stripIniValue: 解析、规范化或格式化对应内容。
 * - formatMacString: 解析、规范化或格式化对应内容。
 * - parseMacString: 解析、规范化或格式化对应内容。
 * - jsonEscape: 解析、规范化或格式化对应内容。
 * - onEspNowSend: 事件回调处理函数。
 * - ensureWhitelistCsvExists: 初始化或确保对应资源可用。
 * - readWhitelistCsvText: 读取、获取或消费对应数据。
 * - saveWhitelistCsvText: 保存、写入或更新对应数据。
 * - parseWhitelistCsv: 解析、规范化或格式化对应内容。
 * - reloadWhitelistFromFat: 模块内部辅助函数。
 * - persistWhitelistEnabledToSettingIni: 保存、写入或更新对应数据。
 * - loadApCredentialsFromSettingIni: 读取、获取或消费对应数据。
 * - persistHostConfigToSettingIni: 保存、写入或更新对应数据。
 * - apRootUrl: 模块内部辅助函数。
 * - redirectToPortal: 绘制界面、输出内容或响应请求。
 * - initEspNowBroadcaster: 初始化或确保对应资源可用。
 * - deinitEspNowBroadcaster: 停止、释放或清理对应状态。
 * - ensureEspNowPeer: 初始化或确保对应资源可用。
 * - buildPacketFromText: 模块内部辅助函数。
 * - sendPacketToMac: 模块内部辅助函数。
 * - findWhitelistEntryByMac: 模块内部辅助函数。
 * - collectSelectedTargets: 模块内部辅助函数。
 * - sendTextByCurrentMode: 模块内部辅助函数。
 * - targetsJson: 模块内部辅助函数。
 * - statusJson: 模块内部辅助函数。
 * - sendPortalPage: 模块内部辅助函数。
 * - registerRoutes: 统一注册无线门户 HTTP 路由。
 * - webServerTask: FreeRTOS 任务入口或任务控制函数。
 * - hostPortalStart: 模块内部辅助函数。
 * - hostPortalStop: 模块内部辅助函数。
 */
#include "HostWirelessPortal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <FFat.h>
#include <FS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <vector>

#include "EspNowMessage.h"

namespace
{

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
constexpr char kWhitelistCsvPath[] = "/espnow_clients.csv";
constexpr size_t kClientNoteMaxLen = 63;

struct WhitelistEntry
{
  uint8_t mac[6];
  char macText[18];
  char note[kClientNoteMaxLen + 1];
};

const char kHostPortalHtml[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP-NOW Host</title>
<style>body{font-family:Segoe UI,Microsoft YaHei,sans-serif;background:#0b1017;color:#f4f8ff;margin:0;padding:12px}.c{background:#182435;border:1px solid #2e4661;border-radius:12px;padding:12px;margin-bottom:10px}textarea,input{width:100%;box-sizing:border-box;border:1px solid #456181;border-radius:8px;background:#0d1825;color:#f4f8ff;padding:10px}textarea{min-height:120px}.r{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-top:8px}button{border:0;border-radius:8px;padding:9px 12px;font-weight:700}button.p{background:#53e4ff;color:#04111a}button.g{background:#2b415c;color:#f4f8ff}.s{min-height:18px;color:#b4c5dd;margin-top:8px;white-space:pre-wrap}.e{color:#ff8f8f}.box{max-height:180px;overflow:auto;border:1px solid #3d5875;border-radius:8px;padding:8px;background:#0f1a28}.it{display:flex;gap:6px;align-items:center;padding:4px 0;border-bottom:1px solid rgba(180,197,221,.15)}.it:last-child{border-bottom:none}</style></head>
<body>
<div class="c"><h3>1) Send</h3><textarea id="msg" maxlength="192" placeholder="Text to send"></textarea>
<div class="r"><label><input id="wl" type="checkbox">Whitelist range</label><label><input id="tg" type="checkbox">Targeted</label></div>
<div id="tbox" class="box" style="display:none"><div id="tlist"></div></div>
<div class="r"><button id="send" class="p">Send</button><button id="clear" class="g">Clear</button></div><p id="ss" class="s"></p></div>
<div class="c"><h3>2) Whitelist CSV (FFat)</h3><div class="r"><button id="lcsv" class="p">Load CSV</button><button id="scsv" class="p">Save CSV</button></div><textarea id="csv" placeholder="MAC,Name&#10;AA:BB:CC:DD:EE:FF,Client-1"></textarea><p class="s">Use client STA MAC for targeted mode.</p><p id="cs" class="s"></p></div>
<div class="c"><h3>3) AP Config</h3><input id="ssid" maxlength="32" placeholder="SSID"><div class="r"></div><input id="pwd" maxlength="63" placeholder="Password (empty=open)"><div class="r"></div><input id="ch" type="number" min="1" max="13" step="1" placeholder="Channel 1-13"><div class="r"><button id="scfg" class="p">Save Config</button></div><p id="cfgs" class="s"></p></div>
<div class="c"><h3>4) Device MAC (STA source)</h3><div class="r"><input id="smac" readonly><button id="cmac" class="g">Copy MAC</button></div><p id="ms" class="s"></p><p id="rs" class="s"></p></div>
<script>
const q=id=>document.getElementById(id),msg=q('msg'),wl=q('wl'),tg=q('tg'),tbox=q('tbox'),tlist=q('tlist'),csv=q('csv'),ssid=q('ssid'),pwd=q('pwd'),ch=q('ch'),smac=q('smac');
function st(el,t,e=false){el.textContent=t||'';el.classList.toggle('e',!!e)}function ut(){tbox.style.display=(wl.checked&&tg.checked)?'block':'none'}
function msel(){return Array.from(document.querySelectorAll('.tc:checked')).map(x=>x.value).join(',')}
async function cp(t){try{if(navigator.clipboard&&window.isSecureContext){await navigator.clipboard.writeText(t);return true}}catch(e){}const a=document.createElement('textarea');a.value=t;a.style.position='fixed';a.style.opacity='0';document.body.appendChild(a);a.focus();a.select();let ok=false;try{ok=document.execCommand('copy')}catch(e){}document.body.removeChild(a);return ok}
async function modeLoad(){try{const r=await fetch('/api/sendmode');const d=await r.json();if(!r.ok)throw new Error('HTTP '+r.status);wl.checked=!!d.whitelistEnabled;tg.checked=!!d.targetedMode;ut()}catch(e){st(q('ss'),'Load mode failed: '+e.message,true)}}
async function modeSave(){try{const b=new FormData();b.append('whitelistEnabled',wl.checked?'1':'0');b.append('targetedMode',tg.checked?'1':'0');const r=await fetch('/api/sendmode',{method:'POST',body:b});const t=await r.text();if(!r.ok)throw new Error(t||('HTTP '+r.status));const d=JSON.parse(t);wl.checked=!!d.whitelistEnabled;tg.checked=!!d.targetedMode;ut();st(q('ss'),'Send mode updated')}catch(e){st(q('ss'),'Save mode failed: '+e.message,true)}}
function tr(items){tlist.innerHTML='';if(!items||!items.length){const p=document.createElement('p');p.className='s';p.textContent='No whitelist clients';tlist.appendChild(p);return}items.forEach((x,i)=>{const l=document.createElement('label');l.className='it';const c=document.createElement('input');c.type='checkbox';c.className='tc';c.value=x.mac;if(i===0)c.checked=true;const s=document.createElement('span');s.textContent=(x.name||'(no name)')+' | '+x.mac;l.appendChild(c);l.appendChild(s);tlist.appendChild(l)})}
async function targets(){try{const r=await fetch('/api/targets');const d=await r.json();if(!r.ok)throw new Error('HTTP '+r.status);tr(d.items||[])}catch(e){st(q('cs'),'Load targets failed: '+e.message,true)}}
async function csvLoad(){try{const r=await fetch('/api/whitelistcsv');const t=await r.text();if(!r.ok)throw new Error(t||('HTTP '+r.status));csv.value=t;st(q('cs'),'CSV loaded')}catch(e){st(q('cs'),'Load CSV failed: '+e.message,true)}}
async function csvSave(){try{const b=new FormData();b.append('content',csv.value);const r=await fetch('/api/whitelistcsv',{method:'POST',body:b});const t=await r.text();if(!r.ok)throw new Error(t||('HTTP '+r.status));st(q('cs'),t||'CSV saved');await targets()}catch(e){st(q('cs'),'Save CSV failed: '+e.message,true)}}
async function send(){try{const b=new FormData();b.append('text',msg.value);if(wl.checked&&tg.checked)b.append('targets',msel());const r=await fetch('/api/send',{method:'POST',body:b});const t=await r.text();if(!r.ok)throw new Error(t||('HTTP '+r.status));st(q('ss'),t||'sent');msg.value=''}catch(e){st(q('ss'),'Send failed: '+e.message,true)}}
async function cfgLoad(){try{const r=await fetch('/api/config');const d=await r.json();if(!r.ok)throw new Error('HTTP '+r.status);ssid.value=d.ssid||'';pwd.value='';ch.value=String(d.channel||1);st(q('cfgs'),d.passwordSet?'Password is set':'Open AP')}catch(e){st(q('cfgs'),'Config load failed: '+e.message,true)}}
async function cfgSave(){try{const b=new FormData();b.append('ssid',ssid.value.trim());b.append('password',pwd.value);b.append('channel',ch.value.trim());const r=await fetch('/api/config',{method:'POST',body:b});const t=await r.text();if(!r.ok)throw new Error(t||('HTTP '+r.status));const d=JSON.parse(t);st(q('cfgs'),'Saved: SSID='+d.ssid+', CH='+d.channel+'. Reboot required.');pwd.value=''}catch(e){st(q('cfgs'),'Config save failed: '+e.message,true)}}
async function rs(){try{const r=await fetch('/api/status');const d=await r.json();if(!r.ok)throw new Error('HTTP '+r.status);smac.value=d.selfMac||d.mac||'';const tx='TX ok:'+String(d.sendOkCount||0)+' fail:'+String(d.sendFailCount||0)+' last:'+(d.lastSendMac||'-')+'/'+((d.lastSendOk===null)?'?' :(d.lastSendOk?'ok':'fail'));st(q('rs'),'SSID:'+d.ssid+' | IP:'+d.ip+' | CH:'+d.channel+' | WL:'+(d.whitelistEnabled?'on':'off')+' | TG:'+(d.targetedMode?'on':'off')+' | N:'+d.whitelistCount+' | '+tx)}catch(e){st(q('rs'),'Status failed: '+e.message,true)}}
q('send').onclick=send;q('clear').onclick=()=>{msg.value='';msg.focus()};q('lcsv').onclick=csvLoad;q('scsv').onclick=csvSave;q('scfg').onclick=cfgSave;
wl.onchange=async()=>{if(!wl.checked)tg.checked=false;ut();await modeSave()};tg.onchange=async()=>{if(tg.checked)wl.checked=true;ut();await modeSave()};
q('cmac').onclick=async()=>{const t=(smac.value||'').trim();if(!t){st(q('ms'),'MAC is empty',true);return}const ok=await cp(t);st(q('ms'),ok?'MAC copied':'Copy failed',!ok)};
(async()=>{await modeLoad();await csvLoad();await targets();await cfgLoad();await rs();setInterval(rs,1000)})();
</script></body></html>
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
bool gWhitelistEnabled = true;
bool gTargetedMode = false;
std::vector<WhitelistEntry> gWhitelistEntries;
portMUX_TYPE gSendStatMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t gSendCbOkCount = 0;
uint32_t gSendCbFailCount = 0;
char gLastSendMacText[18] = {0};
int gLastSendStatus = -1; // -1 unknown, 0 fail, 1 success

void copyStringToBuf(const String &src, char *dst, size_t dstSize)
{
  if (!dst || dstSize == 0)
    return;
  if (!src.length())
  {
    dst[0] = '\0';
    return;
  }
  src.toCharArray(dst, dstSize);
  dst[dstSize - 1] = '\0';
}

uint8_t clampApChannel(int channel)
{
  if (channel < static_cast<int>(kApChannelMin))
    return kApChannelMin;
  if (channel > static_cast<int>(kApChannelMax))
    return kApChannelMax;
  return static_cast<uint8_t>(channel);
}

bool parseApChannel(const String &raw, uint8_t &outChannel)
{
  String s = raw;
  s.trim();
  if (!s.length())
    return false;
  const int channel = s.toInt();
  if (channel < static_cast<int>(kApChannelMin) || channel > static_cast<int>(kApChannelMax))
    return false;
  outChannel = static_cast<uint8_t>(channel);
  return true;
}

bool parseBoolString(const String &raw, bool &outValue)
{
  String v = raw;
  v.trim();
  v.toLowerCase();
  if (!v.length())
    return false;
  if (v == "1" || v == "true" || v == "on" || v == "yes" || v == "enable" || v == "enabled")
  {
    outValue = true;
    return true;
  }
  if (v == "0" || v == "false" || v == "off" || v == "no" || v == "disable" || v == "disabled")
  {
    outValue = false;
    return true;
  }
  return false;
}

bool forceWifiChannel(uint8_t channel)
{
  if (channel < kApChannelMin || channel > kApChannelMax)
  {
    Serial.printf("[HOST][ESPNOW] invalid channel: %u\n", static_cast<unsigned int>(channel));
    return false;
  }
  const esp_err_t setRet = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  if (setRet != ESP_OK)
  {
    Serial.printf("[HOST][ESPNOW] esp_wifi_set_channel failed: %d\n", static_cast<int>(setRet));
    return false;
  }
  uint8_t primary = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&primary, &second) == ESP_OK)
  {
    Serial.printf("[HOST][ESPNOW] channel locked to %u\n", static_cast<unsigned int>(primary));
  }
  return true;
}

String stripIniValue(String value)
{
  value.trim();
  const int semicolon = value.indexOf(';');
  if (semicolon >= 0)
    value = value.substring(0, semicolon);
  const int hash = value.indexOf('#');
  if (hash >= 0)
    value = value.substring(0, hash);
  value.trim();
  if (value.length() >= 2)
  {
    const char first = value[0];
    const char last = value[value.length() - 1];
    if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
    {
      value = value.substring(1, value.length() - 1);
      value.trim();
    }
  }
  return value;
}

String formatMacString(const uint8_t mac[6])
{
  char buf[18] = {0};
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool parseMacString(const String &text, uint8_t outMac[6])
{
  if (!outMac)
    return false;
  String s = text;
  s.trim();
  if (!s.length())
    return false;

  unsigned int b[6] = {0};
  int n = sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
  if (n != 6)
    n = sscanf(s.c_str(), "%x-%x-%x-%x-%x-%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
  if (n != 6)
    return false;

  for (int i = 0; i < 6; ++i)
  {
    if (b[i] > 0xFFU)
      return false;
    outMac[i] = static_cast<uint8_t>(b[i]);
  }
  return true;
}

String jsonEscape(const String &src)
{
  String out;
  out.reserve(src.length() + 8);
  for (size_t i = 0; i < src.length(); ++i)
  {
    const char c = src[i];
    switch (c)
    {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20U)
      {
        char buf[7];
        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(c)));
        out += buf;
      }
      else
      {
        out += c;
      }
      break;
    }
  }
  return out;
}

void onEspNowSend(const uint8_t *macAddr, esp_now_send_status_t status)
{
  char macText[18] = {0};
  if (macAddr)
  {
    snprintf(macText, sizeof(macText), "%02X:%02X:%02X:%02X:%02X:%02X", macAddr[0], macAddr[1], macAddr[2], macAddr[3],
             macAddr[4], macAddr[5]);
  }
  else
  {
    strncpy(macText, "<NULL>", sizeof(macText) - 1);
    macText[sizeof(macText) - 1] = '\0';
  }

  portENTER_CRITICAL(&gSendStatMux);
  memcpy(gLastSendMacText, macText, sizeof(gLastSendMacText));
  gLastSendStatus = (status == ESP_NOW_SEND_SUCCESS) ? 1 : 0;
  if (status == ESP_NOW_SEND_SUCCESS)
  {
    ++gSendCbOkCount;
  }
  else
  {
    ++gSendCbFailCount;
  }
  portEXIT_CRITICAL(&gSendStatMux);
}

bool ensureWhitelistCsvExists()
{
  if (FFat.exists(kWhitelistCsvPath))
    return true;
  fs::File f = FFat.open(kWhitelistCsvPath, "w");
  if (!f)
    return false;
  const String content = "MAC,Name\n";
  const size_t written = f.print(content);
  f.close();
  return written == content.length();
}

String readWhitelistCsvText()
{
  fs::File f = FFat.open(kWhitelistCsvPath, FILE_READ);
  if (!f)
    return "";
  String content = f.readString();
  f.close();
  return content;
}

bool saveWhitelistCsvText(const String &content)
{
  fs::File f = FFat.open(kWhitelistCsvPath, "w");
  if (!f)
    return false;
  const size_t written = f.print(content);
  f.close();
  return written == content.length();
}

bool parseWhitelistCsv(const String &csvText, std::vector<WhitelistEntry> &outEntries, String &errorOut,
                       bool strictMode)
{
  outEntries.clear();
  errorOut = "";

  int start = 0;
  int lineNo = 0;
  while (start <= csvText.length())
  {
    const int end = csvText.indexOf('\n', start);
    String line = (end >= 0) ? csvText.substring(start, end) : csvText.substring(start);
    if (line.endsWith("\r"))
      line.remove(line.length() - 1);
    ++lineNo;

    String trimmed = line;
    trimmed.trim();
    if (trimmed.length() == 0 || trimmed.startsWith("#") || trimmed.startsWith(";"))
    {
      if (end >= 0)
      {
        start = end + 1;
        continue;
      }
      break;
    }

    String lower = trimmed;
    lower.toLowerCase();
    if (lineNo == 1 && lower.indexOf("mac") >= 0)
    {
      if (end >= 0)
      {
        start = end + 1;
        continue;
      }
      break;
    }

    const int comma = line.indexOf(',');
    String macText = (comma >= 0) ? line.substring(0, comma) : line;
    String note = (comma >= 0) ? line.substring(comma + 1) : "";
    macText.trim();
    note.trim();

    if (!macText.length())
    {
      if (strictMode)
      {
        errorOut = "line " + String(lineNo) + ": empty MAC";
        return false;
      }
      if (end >= 0)
      {
        start = end + 1;
        continue;
      }
      break;
    }

    uint8_t mac[6] = {0};
    if (!parseMacString(macText, mac))
    {
      if (strictMode)
      {
        errorOut = "line " + String(lineNo) + ": invalid MAC format";
        return false;
      }
      if (end >= 0)
      {
        start = end + 1;
        continue;
      }
      break;
    }

    bool existed = false;
    for (size_t i = 0; i < outEntries.size(); ++i)
    {
      if (memcmp(outEntries[i].mac, mac, 6) == 0)
      {
        copyStringToBuf(note, outEntries[i].note, sizeof(outEntries[i].note));
        existed = true;
        break;
      }
    }

    if (!existed)
    {
      WhitelistEntry entry = {};
      memcpy(entry.mac, mac, 6);
      copyStringToBuf(formatMacString(mac), entry.macText, sizeof(entry.macText));
      copyStringToBuf(note, entry.note, sizeof(entry.note));
      outEntries.push_back(entry);
    }

    if (end >= 0)
    {
      start = end + 1;
    }
    else
    {
      break;
    }
  }

  return true;
}

bool reloadWhitelistFromFat(String &errorOut)
{
  if (!ensureWhitelistCsvExists())
  {
    errorOut = "cannot create whitelist csv";
    return false;
  }

  std::vector<WhitelistEntry> parsed;
  String parseError;
  if (!parseWhitelistCsv(readWhitelistCsvText(), parsed, parseError, false))
  {
    errorOut = parseError;
    return false;
  }
  gWhitelistEntries = parsed;
  return true;
}

bool persistWhitelistEnabledToSettingIni(bool enabled)
{
  String original;
  if (FFat.exists("/setting.ini"))
  {
    fs::File rf = FFat.open("/setting.ini", FILE_READ);
    if (!rf)
      return false;
    original = rf.readString();
    rf.close();
  }

  bool found = false;
  String output;
  output.reserve(original.length() + 48);

  int start = 0;
  while (start <= original.length())
  {
    const int end = original.indexOf('\n', start);
    String line = (end >= 0) ? original.substring(start, end) : original.substring(start);

    String trimmed = line;
    trimmed.trim();
    if (trimmed.length() && !trimmed.startsWith("#") && !trimmed.startsWith(";"))
    {
      const int eq = trimmed.indexOf('=');
      if (eq > 0)
      {
        String key = trimmed.substring(0, eq);
        key.trim();
        key.toLowerCase();
        if (key == "hostwhitelistenabled")
        {
          line = String("HostWhitelistEnabled = ") + (enabled ? "true;" : "false;");
          found = true;
        }
      }
    }

    output += line;
    if (end >= 0)
    {
      output += '\n';
      start = end + 1;
    }
    else
    {
      break;
    }
  }

  if (!found)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += String("HostWhitelistEnabled = ") + (enabled ? "true;\n" : "false;\n");
  }

  fs::File wf = FFat.open("/setting.ini", "w");
  if (!wf)
    return false;
  const size_t written = wf.print(output);
  wf.close();
  return written == output.length();
}

void loadApCredentialsFromSettingIni()
{
  copyStringToBuf(String(kDefaultApSsid), gApSsid, sizeof(gApSsid));
  copyStringToBuf(String(kDefaultApPassword), gApPassword, sizeof(gApPassword));
  gApChannel = kDefaultApChannel;
  gWhitelistEnabled = true;

  fs::File f = FFat.open("/setting.ini", FILE_READ);
  if (!f)
  {
    Serial.println("[HOST] /setting.ini not found, using default AP config");
    return;
  }

  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length())
      continue;
    if (line.startsWith("#") || line.startsWith(";"))
      continue;

    const int eq = line.indexOf('=');
    if (eq <= 0)
      continue;

    String key = line.substring(0, eq);
    String value = line.substring(eq + 1);
    key.trim();
    key.toLowerCase();
    value = stripIniValue(value);

    if (key == "ssip" || key == "ssid")
    {
      if (value.length())
        copyStringToBuf(value, gApSsid, sizeof(gApSsid));
    }
    else if (key == "password")
    {
      if (!value.length())
      {
        gApPassword[0] = '\0';
      }
      else if (value.length() >= 8)
      {
        copyStringToBuf(value, gApPassword, sizeof(gApPassword));
      }
      else
      {
        gApPassword[0] = '\0';
      }
    }
    else if (key == "espnowchannel" || key == "channel" || key == "apchannel")
    {
      uint8_t parsed = kDefaultApChannel;
      if (parseApChannel(value, parsed))
        gApChannel = parsed;
    }
    else if (key == "hostwhitelistenabled")
    {
      bool parsed = true;
      if (parseBoolString(value, parsed))
        gWhitelistEnabled = parsed;
    }
  }
  f.close();
}

bool persistHostConfigToSettingIni(const String &ssidRaw, const String &passwordRaw, uint8_t channel)
{
  String ssid = ssidRaw;
  String password = passwordRaw;
  ssid.trim();
  password.trim();
  channel = clampApChannel(channel);

  if (!ssid.length())
    return false;
  if (ssid.length() >= sizeof(gApSsid))
    return false;
  if (password.length() && (password.length() < 8 || password.length() > 63))
    return false;

  String original;
  if (FFat.exists("/setting.ini"))
  {
    fs::File rf = FFat.open("/setting.ini", FILE_READ);
    if (!rf)
      return false;
    original = rf.readString();
    rf.close();
  }

  bool foundSsid = false;
  bool foundPassword = false;
  bool foundChannel = false;
  bool foundWhitelist = false;
  String output;
  output.reserve(original.length() + 160);

  int start = 0;
  while (start <= original.length())
  {
    const int end = original.indexOf('\n', start);
    String line = (end >= 0) ? original.substring(start, end) : original.substring(start);

    String trimmed = line;
    trimmed.trim();
    if (trimmed.length() && !trimmed.startsWith("#") && !trimmed.startsWith(";"))
    {
      const int eq = trimmed.indexOf('=');
      if (eq > 0)
      {
        String key = trimmed.substring(0, eq);
        key.trim();
        key.toLowerCase();
        if (key == "ssip" || key == "ssid")
        {
          line = "SSID = \"" + ssid + "\";";
          foundSsid = true;
        }
        else if (key == "password")
        {
          line = "Password = \"" + password + "\";";
          foundPassword = true;
        }
        else if (key == "espnowchannel" || key == "channel" || key == "apchannel")
        {
          line = "EspNowChannel = " + String(static_cast<unsigned int>(channel)) + ";";
          foundChannel = true;
        }
        else if (key == "hostwhitelistenabled")
        {
          line = String("HostWhitelistEnabled = ") + (gWhitelistEnabled ? "true;" : "false;");
          foundWhitelist = true;
        }
      }
    }

    output += line;
    if (end >= 0)
    {
      output += '\n';
      start = end + 1;
    }
    else
    {
      break;
    }
  }

  if (!foundSsid)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "SSID = \"" + ssid + "\";\n";
  }
  if (!foundPassword)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "Password = \"" + password + "\";\n";
  }
  if (!foundChannel)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "EspNowChannel = " + String(static_cast<unsigned int>(channel)) + ";\n";
  }
  if (!foundWhitelist)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += String("HostWhitelistEnabled = ") + (gWhitelistEnabled ? "true;\n" : "false;\n");
  }

  fs::File wf = FFat.open("/setting.ini", "w");
  if (!wf)
    return false;
  const size_t written = wf.print(output);
  wf.close();
  return written == output.length();
}

String apRootUrl()
{
  String url = "http://";
  url += WiFi.softAPIP().toString();
  url += "/";
  return url;
}

void redirectToPortal()
{
  gWebServer->sendHeader("Location", apRootUrl(), true);
  gWebServer->send(302, "text/plain", "");
}

bool initEspNowBroadcaster()
{
  if (gEspNowReady)
    return true;
  if (esp_now_init() != ESP_OK)
  {
    Serial.println("[HOST][ESPNOW] init failed");
    return false;
  }
  if (esp_now_register_send_cb(onEspNowSend) != ESP_OK)
  {
    Serial.println("[HOST][ESPNOW] register send callback failed");
    esp_now_deinit();
    return false;
  }

  uint8_t broadcastAddr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddr, sizeof(broadcastAddr));
  peer.channel = gApChannel;
  peer.encrypt = false;
  // Use STA interface as the unified ESPNOW source for both broadcast and targeted send.
  peer.ifidx = WIFI_IF_STA;
  const esp_err_t addRet = esp_now_add_peer(&peer);
  if (addRet != ESP_OK && addRet != ESP_ERR_ESPNOW_EXIST)
  {
    Serial.printf("[HOST][ESPNOW] add broadcast peer failed: %d\n", static_cast<int>(addRet));
    esp_now_deinit();
    return false;
  }

  gEspNowReady = true;
  Serial.println("[HOST][ESPNOW] broadcaster ready");
  return true;
}

void deinitEspNowBroadcaster()
{
  if (!gEspNowReady)
    return;
  esp_now_unregister_send_cb();
  esp_now_deinit();
  gEspNowReady = false;
}

bool ensureEspNowPeer(const uint8_t mac[6], String &errorOut)
{
  if (esp_now_is_peer_exist(mac))
  {
    esp_now_peer_info_t existing = {};
    const esp_err_t getRet = esp_now_get_peer(mac, &existing);
    if (getRet == ESP_OK && existing.ifidx == WIFI_IF_STA)
      return true;

    // Re-target existing peer to STA interface for stable client STA-MAC unicast.
    if (getRet == ESP_OK)
    {
      existing.channel = gApChannel;
      existing.encrypt = false;
      existing.ifidx = WIFI_IF_STA;
      const esp_err_t modRet = esp_now_mod_peer(&existing);
      if (modRet == ESP_OK)
        return true;
    }
    esp_now_del_peer(mac);
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = gApChannel;
  peer.encrypt = false;
  // Targeted send should use STA interface to reach client STA MAC
  // regardless of receiver AP toggle state.
  peer.ifidx = WIFI_IF_STA;
  const esp_err_t ret = esp_now_add_peer(&peer);
  if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST)
  {
    errorOut = "add peer failed";
    return false;
  }
  return true;
}

bool buildPacketFromText(const String &rawText, EspNowTextPacket &pkt, String &errorOut)
{
  String text = rawText;
  text.trim();
  if (!text.length())
  {
    errorOut = "text is empty";
    return false;
  }
  if (text.length() > kEspNowTextMaxBytes - 1)
    text.remove(kEspNowTextMaxBytes - 1);

  pkt = {};
  pkt.magic = kEspNowTextMagic;
  pkt.type = kEspNowMsgTypeText;
  pkt.seq = gEspNowSeq++;
  text.toCharArray(pkt.text, sizeof(pkt.text));
  return true;
}

bool sendPacketToMac(const uint8_t mac[6], const EspNowTextPacket &pkt, String &errorOut)
{
  if (!ensureEspNowPeer(mac, errorOut))
    return false;
  esp_now_peer_info_t info = {};
  if (esp_now_get_peer(mac, &info) == ESP_OK)
  {
    Serial.printf("[HOST][ESPNOW] tx target=%s if=%d ch=%u\n", formatMacString(mac).c_str(),
                  static_cast<int>(info.ifidx), static_cast<unsigned int>(info.channel));
  }
  const esp_err_t ret = esp_now_send(mac, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt));
  if (ret != ESP_OK)
  {
    errorOut = "esp-now send failed(" + String(static_cast<int>(ret)) + ")";
    Serial.printf("[HOST][ESPNOW] tx fail target=%s err=%d\n", formatMacString(mac).c_str(), static_cast<int>(ret));
    return false;
  }
  Serial.printf("[HOST][ESPNOW] tx queued target=%s\n", formatMacString(mac).c_str());
  return true;
}

bool findWhitelistEntryByMac(const uint8_t mac[6], WhitelistEntry &outEntry)
{
  for (size_t i = 0; i < gWhitelistEntries.size(); ++i)
  {
    if (memcmp(gWhitelistEntries[i].mac, mac, 6) == 0)
    {
      outEntry = gWhitelistEntries[i];
      return true;
    }
  }
  return false;
}

bool collectSelectedTargets(const String &targetsRaw, std::vector<WhitelistEntry> &outTargets, String &errorOut)
{
  outTargets.clear();

  String normalized = targetsRaw;
  normalized.replace(";", ",");
  normalized.replace("\n", ",");
  normalized.replace("\r", ",");

  int start = 0;
  while (start <= normalized.length())
  {
    const int end = normalized.indexOf(',', start);
    String token = (end >= 0) ? normalized.substring(start, end) : normalized.substring(start);
    token.trim();

    if (token.length())
    {
      uint8_t mac[6] = {0};
      if (!parseMacString(token, mac))
      {
        errorOut = "invalid target MAC: " + token;
        return false;
      }

      WhitelistEntry entry = {};
      if (!findWhitelistEntryByMac(mac, entry))
      {
        errorOut = "target not in whitelist: " + token;
        return false;
      }

      bool exists = false;
      for (size_t i = 0; i < outTargets.size(); ++i)
      {
        if (memcmp(outTargets[i].mac, entry.mac, 6) == 0)
        {
          exists = true;
          break;
        }
      }
      if (!exists)
        outTargets.push_back(entry);
    }

    if (end >= 0)
    {
      start = end + 1;
    }
    else
    {
      break;
    }
  }

  if (outTargets.empty())
  {
    errorOut = "no target selected";
    return false;
  }
  return true;
}

bool sendTextByCurrentMode(const String &rawText, const String &targetsRaw, String &resultOut, String &errorOut)
{
  if (!gEspNowReady)
  {
    errorOut = "esp-now not ready";
    return false;
  }

  EspNowTextPacket pkt = {};
  if (!buildPacketFromText(rawText, pkt, errorOut))
    return false;

  if (!gWhitelistEnabled)
  {
    uint8_t broadcastAddr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const esp_err_t ret = esp_now_send(broadcastAddr, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt));
    if (ret != ESP_OK)
    {
      errorOut = "broadcast send failed(" + String(static_cast<int>(ret)) + ")";
      return false;
    }
    resultOut = "broadcast sent to all (whitelist OFF)";
    return true;
  }

  if (gWhitelistEntries.empty())
  {
    errorOut = "whitelist is empty";
    return false;
  }

  std::vector<WhitelistEntry> targets;
  if (gTargetedMode)
  {
    if (!collectSelectedTargets(targetsRaw, targets, errorOut))
      return false;
  }
  else
  {
    targets = gWhitelistEntries;
  }

  int okCount = 0;
  int failCount = 0;
  String lastFail;
  for (size_t i = 0; i < targets.size(); ++i)
  {
    String err;
    if (sendPacketToMac(targets[i].mac, pkt, err))
    {
      ++okCount;
    }
    else
    {
      ++failCount;
      lastFail = targets[i].macText;
      if (err.length())
        lastFail += " (" + err + ")";
    }
  }

  if (okCount == 0)
  {
    errorOut = "send failed";
    if (lastFail.length())
      errorOut += ": " + lastFail;
    return false;
  }

  resultOut = "sent " + String(okCount) + "/" + String(static_cast<unsigned int>(targets.size()));
  if (failCount > 0)
    resultOut += ", failed=" + String(failCount);
  return true;
}

String targetsJson()
{
  String out = "{\"count\":";
  out += String(static_cast<unsigned int>(gWhitelistEntries.size()));
  out += ",\"items\":[";
  for (size_t i = 0; i < gWhitelistEntries.size(); ++i)
  {
    if (i > 0)
      out += ",";
    out += "{\"mac\":\"";
    out += gWhitelistEntries[i].macText;
    out += "\",\"name\":\"";
    out += jsonEscape(String(gWhitelistEntries[i].note));
    out += "\"}";
  }
  out += "]}";
  return out;
}

String statusJson()
{
  const String staMac = WiFi.macAddress();
  const String apMac = WiFi.softAPmacAddress();
  uint32_t sendOk = 0;
  uint32_t sendFail = 0;
  int lastSend = -1;
  char lastSendMac[18] = {0};
  portENTER_CRITICAL(&gSendStatMux);
  sendOk = gSendCbOkCount;
  sendFail = gSendCbFailCount;
  lastSend = gLastSendStatus;
  memcpy(lastSendMac, gLastSendMacText, sizeof(lastSendMac));
  portEXIT_CRITICAL(&gSendStatMux);

  String out = "{\"ip\":\"";
  out += WiFi.softAPIP().toString();
  out += "\",\"ssid\":\"";
  out += String(gApSsid);
  out += "\",\"mac\":\"";
  out += apMac;
  out += "\",\"selfMac\":\"";
  out += staMac;
  out += "\",\"selfStaMac\":\"";
  out += staMac;
  out += "\",\"selfApMac\":\"";
  out += apMac;
  out += "\",\"channel\":";
  out += String(WiFi.channel());
  out += ",\"passwordSet\":";
  out += gApPassword[0] ? "true" : "false";
  out += ",\"whitelistEnabled\":";
  out += gWhitelistEnabled ? "true" : "false";
  out += ",\"targetedMode\":";
  out += gTargetedMode ? "true" : "false";
  out += ",\"whitelistCount\":";
  out += String(static_cast<unsigned int>(gWhitelistEntries.size()));
  out += ",\"sendOkCount\":";
  out += String(static_cast<unsigned long>(sendOk));
  out += ",\"sendFailCount\":";
  out += String(static_cast<unsigned long>(sendFail));
  out += ",\"lastSendMac\":\"";
  out += String(lastSendMac);
  out += "\",\"lastSendOk\":";
  if (lastSend < 0)
  {
    out += "null";
  }
  else
  {
    out += (lastSend > 0) ? "true" : "false";
  }
  out += "}";
  return out;
}

void sendPortalPage()
{
  gWebServer->send_P(200, "text/html; charset=utf-8", kHostPortalHtml);
}

void registerRoutes()
{
  gWebServer->on("/", HTTP_GET, []() { sendPortalPage(); });
  gWebServer->on("/index.html", HTTP_GET, []() { sendPortalPage(); });

  gWebServer->on("/api/status", HTTP_GET, []() { gWebServer->send(200, "application/json", statusJson()); });

  gWebServer->on("/api/sendmode", HTTP_GET,
                 []()
                 {
                   String out = "{\"whitelistEnabled\":";
                   out += gWhitelistEnabled ? "true" : "false";
                   out += ",\"targetedMode\":";
                   out += gTargetedMode ? "true" : "false";
                   out += "}";
                   gWebServer->send(200, "application/json", out);
                 });

  gWebServer->on("/api/sendmode", HTTP_POST,
                 []()
                 {
                   bool nextWhitelist = gWhitelistEnabled;
                   bool nextTargeted = gTargetedMode;

                   if (gWebServer->hasArg("whitelistEnabled"))
                   {
                     if (!parseBoolString(gWebServer->arg("whitelistEnabled"), nextWhitelist))
                     {
                       gWebServer->send(400, "text/plain", "invalid whitelistEnabled");
                       return;
                     }
                   }
                   if (gWebServer->hasArg("targetedMode"))
                   {
                     if (!parseBoolString(gWebServer->arg("targetedMode"), nextTargeted))
                     {
                       gWebServer->send(400, "text/plain", "invalid targetedMode");
                       return;
                     }
                   }

                   if (!nextWhitelist)
                     nextTargeted = false;
                   if (nextWhitelist != gWhitelistEnabled)
                   {
                     if (!persistWhitelistEnabledToSettingIni(nextWhitelist))
                     {
                       gWebServer->send(500, "text/plain", "save /setting.ini failed");
                       return;
                     }
                   }

                   gWhitelistEnabled = nextWhitelist;
                   gTargetedMode = nextTargeted;

                   String out = "{\"whitelistEnabled\":";
                   out += gWhitelistEnabled ? "true" : "false";
                   out += ",\"targetedMode\":";
                   out += gTargetedMode ? "true" : "false";
                   out += "}";
                   gWebServer->send(200, "application/json", out);
                 });

  gWebServer->on("/api/targets", HTTP_GET, []() { gWebServer->send(200, "application/json", targetsJson()); });

  gWebServer->on("/api/whitelistcsv", HTTP_GET,
                 []()
                 {
                   if (!ensureWhitelistCsvExists())
                   {
                     gWebServer->send(500, "text/plain", "cannot create whitelist csv");
                     return;
                   }
                   gWebServer->send(200, "text/plain; charset=utf-8", readWhitelistCsvText());
                 });

  gWebServer->on("/api/whitelistcsv", HTTP_POST,
                 []()
                 {
                   String content = gWebServer->arg("content");
                   if (!content.length() && gWebServer->hasArg("plain"))
                     content = gWebServer->arg("plain");

                   std::vector<WhitelistEntry> parsed;
                   String parseError;
                   if (!parseWhitelistCsv(content, parsed, parseError, true))
                   {
                     gWebServer->send(400, "text/plain", parseError);
                     return;
                   }
                   if (!saveWhitelistCsvText(content))
                   {
                     gWebServer->send(500, "text/plain", "save whitelist csv failed");
                     return;
                   }

                   gWhitelistEntries = parsed;
                   String out =
                       "saved whitelist csv, entries=" + String(static_cast<unsigned int>(gWhitelistEntries.size()));
                   gWebServer->send(200, "text/plain", out);
                 });

  gWebServer->on("/api/send", HTTP_POST,
                 []()
                 {
                   String text = gWebServer->arg("text");
                   if (!text.length() && gWebServer->hasArg("plain"))
                     text = gWebServer->arg("plain");
                   const String targets = gWebServer->arg("targets");

                   String result;
                   String error;
                   if (!sendTextByCurrentMode(text, targets, result, error))
                   {
                     gWebServer->send(400, "text/plain", error.length() ? error : "send failed");
                     return;
                   }
                   gWebServer->send(200, "text/plain", result);
                 });

  gWebServer->on("/api/config", HTTP_GET,
                 []()
                 {
                   String out = "{\"ssid\":\"";
                   out += String(gApSsid);
                   out += "\",\"passwordSet\":";
                   out += gApPassword[0] ? "true" : "false";
                   out += ",\"channel\":";
                   out += String(static_cast<unsigned int>(gApChannel));
                   out += "}";
                   gWebServer->send(200, "application/json", out);
                 });

  gWebServer->on("/api/config", HTTP_POST,
                 []()
                 {
                   String ssid = gWebServer->arg("ssid");
                   if (!ssid.length() && gWebServer->hasArg("ssip"))
                     ssid = gWebServer->arg("ssip");
                   if (!ssid.length() && gWebServer->hasArg("plain"))
                     ssid = gWebServer->arg("plain");
                   String password = gWebServer->arg("password");
                   String channelArg = gWebServer->arg("channel");

                   ssid.trim();
                   password.trim();
                   channelArg.trim();

                   if (!ssid.length())
                   {
                     gWebServer->send(400, "text/plain", "ssid is empty");
                     return;
                   }
                   if (ssid.length() >= sizeof(gApSsid))
                   {
                     gWebServer->send(400, "text/plain", "ssid too long");
                     return;
                   }
                   if (password.length() && (password.length() < 8 || password.length() > 63))
                   {
                     gWebServer->send(400, "text/plain", "password must be empty or 8-63 chars");
                     return;
                   }

                   uint8_t nextChannel = gApChannel;
                   if (channelArg.length() && !parseApChannel(channelArg, nextChannel))
                   {
                     gWebServer->send(400, "text/plain", "channel must be 1-13");
                     return;
                   }

                   if (!persistHostConfigToSettingIni(ssid, password, nextChannel))
                   {
                     gWebServer->send(500, "text/plain", "save /setting.ini failed");
                     return;
                   }

                   copyStringToBuf(ssid, gApSsid, sizeof(gApSsid));
                   if (!password.length())
                   {
                     gApPassword[0] = '\0';
                   }
                   else
                   {
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

  auto captiveRedirect = []() { redirectToPortal(); };
  gWebServer->on("/generate_204", HTTP_GET, captiveRedirect);
  gWebServer->on("/gen_204", HTTP_GET, captiveRedirect);
  gWebServer->on("/hotspot-detect.html", HTTP_GET, captiveRedirect);
  gWebServer->on("/connecttest.txt", HTTP_GET, captiveRedirect);
  gWebServer->on("/ncsi.txt", HTTP_GET, captiveRedirect);
  gWebServer->on("/fwlink", HTTP_GET, captiveRedirect);

  gWebServer->onNotFound(
      []()
      {
        const String uri = gWebServer->uri();
        if (uri.startsWith("/api/"))
        {
          gWebServer->send(404, "text/plain", "not found");
          return;
        }
        redirectToPortal();
      });
}

void webServerTask(void *param)
{
  (void)param;
  gWebServer = new WebServer(kHttpPort);
  gDnsServer = new DNSServer();
  if (!gWebServer || !gDnsServer)
  {
    if (gDnsServer)
    {
      delete gDnsServer;
      gDnsServer = nullptr;
    }
    if (gWebServer)
    {
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

  while (gWebTaskRunning)
  {
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

} // namespace

bool hostPortalStart()
{
  if (gPortalStarted)
    return true;

  loadApCredentialsFromSettingIni();
  String wlError;
  if (!reloadWhitelistFromFat(wlError))
  {
    Serial.printf("[HOST] whitelist load failed: %s\n", wlError.c_str());
  }

  WiFi.mode(WIFI_AP_STA);
  bool apOk = false;
  if (gApPassword[0] == '\0')
  {
    apOk = WiFi.softAP(gApSsid, nullptr, gApChannel);
  }
  else
  {
    apOk = WiFi.softAP(gApSsid, gApPassword, gApChannel);
  }
  if (!apOk)
  {
    Serial.println("[HOST][WEB] softAP start failed");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  if (!forceWifiChannel(gApChannel))
  {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  if (!initEspNowBroadcaster())
  {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  gWebTaskRunning = true;
  const BaseType_t ok = xTaskCreatePinnedToCore(webServerTask, "HostWebPortal", kWebTaskStack, nullptr,
                                                kWebTaskPriority, &gWebTaskHandle, kWebTaskCore);
  if (ok != pdPASS)
  {
    gWebTaskRunning = false;
    deinitEspNowBroadcaster();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[HOST][WEB] task create failed");
    return false;
  }

  gPortalStarted = true;
  Serial.printf("[HOST] AP started SSID=%s PASS=%s IP=%s CH=%d AP_MAC=%s STA_MAC=%s WL=%d TARGET=%d CSV=%s count=%u\n",
                gApSsid, gApPassword[0] ? gApPassword : "<OPEN>", WiFi.softAPIP().toString().c_str(), WiFi.channel(),
                WiFi.softAPmacAddress().c_str(), WiFi.macAddress().c_str(), gWhitelistEnabled ? 1 : 0,
                gTargetedMode ? 1 : 0, kWhitelistCsvPath, static_cast<unsigned int>(gWhitelistEntries.size()));
  return true;
}

void hostPortalStop()
{
  if (!gPortalStarted)
    return;

  gWebTaskRunning = false;
  for (int i = 0; i < 120 && gWebTaskHandle != nullptr; ++i)
  {
    delay(5);
  }
  if (gWebTaskHandle != nullptr)
  {
    vTaskDelete(gWebTaskHandle);
    gWebTaskHandle = nullptr;
  }
  if (gWebServer)
  {
    gWebServer->stop();
    delete gWebServer;
    gWebServer = nullptr;
  }
  if (gDnsServer)
  {
    gDnsServer->stop();
    delete gDnsServer;
    gDnsServer = nullptr;
  }

  deinitEspNowBroadcaster();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  gPortalStarted = false;
}
