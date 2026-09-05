/**
 * web_config.ino
 * Web config portal - ported from web_config.cpp
 * Classic ESP32 compatible. Runs WebServer on port 80.
 * Access at http://<device-ip>/ or http://wireclaw.local/
 */

#include "config.h"
#include <LittleFS.h>  /* ensure File/LittleFS visible in C-mode compile */
#include <WebServer.h>
#include <ESPmDNS.h>

static WebServer wcServer(80);

static int wcReadFile(const char *path, char *buf, int buf_len) {
    File f = LittleFS.open(path, "r");
    if (!f) return -1;
    int len = f.readBytes(buf, buf_len - 1);
    buf[len] = '\0';
    f.close();
    return len;
}

static bool wcJsonGetString(const char *json, const char *key,
                             char *dst, int dst_len) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    int w = 0;
    while (*p && *p != '"' && w < dst_len - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        dst[w++] = *p++;
    }
    dst[w] = '\0';
    return w > 0;
}

static int jsonEscapeBuf(char *dst, int dst_len, const char *src) {
    int w = 0;
    for (int i = 0; src[i] && w < dst_len - 2; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') { dst[w++] = '\\'; dst[w++] = c; }
        else if (c == '\n') { dst[w++] = '\\'; dst[w++] = 'n'; }
        else if (c == '\r' || (uint8_t)c < 0x20) { /* skip */ }
        else dst[w++] = c;
    }
    dst[w] = '\0';
    return w;
}

static void maskSensitive(const char *src, char *dst, int dst_len) {
    int len = strlen(src);
    if (len == 0) { dst[0] = '\0'; return; }
    if (len <= 4) snprintf(dst, dst_len, "****");
    else          snprintf(dst, dst_len, "...%s", src + len - 4);
}

static bool isMasked(const char *val) {
    if (!val || !val[0]) return false;
    return strncmp(val, "...", 3) == 0 || strncmp(val, "****", 4) == 0;
}

static void handleGetConfig() {
    static char buf[1024];
    char masked_key[16], masked_pass[16];
    maskSensitive(cfg_api_key,   masked_key,  sizeof(masked_key));
    maskSensitive(cfg_wifi_pass, masked_pass, sizeof(masked_pass));
    snprintf(buf, sizeof(buf),
        "{\"wifi_ssid\":\"%s\",\"wifi_pass\":\"%s\",\"api_key\":\"%s\","
        "\"model\":\"%s\",\"device_name\":\"%s\","
        "\"api_base_url\":\"%s\",\"timezone\":\"%s\"}",
        cfg_wifi_ssid, masked_pass, masked_key,
        cfg_model, cfg_device_name, cfg_api_base_url, cfg_timezone);
    wcServer.send(200, "application/json", buf);
}

static void handlePostConfig() {
    if (!wcServer.hasArg("plain")) {
        wcServer.send(400, "application/json", "{\"error\":\"no body\"}"); return;
    }
    String body = wcServer.arg("plain");
    const char *json = body.c_str();
    char tmp[128];
    if (wcJsonGetString(json, "wifi_ssid",    tmp, sizeof(tmp))) strncpy(cfg_wifi_ssid,    tmp, sizeof(cfg_wifi_ssid)-1);
    if (wcJsonGetString(json, "wifi_pass",    tmp, sizeof(tmp)) && !isMasked(tmp)) strncpy(cfg_wifi_pass, tmp, sizeof(cfg_wifi_pass)-1);
    if (wcJsonGetString(json, "api_key",      tmp, sizeof(tmp)) && !isMasked(tmp)) strncpy(cfg_api_key,   tmp, sizeof(cfg_api_key)-1);
    if (wcJsonGetString(json, "model",        tmp, sizeof(tmp))) strncpy(cfg_model,        tmp, sizeof(cfg_model)-1);
    if (wcJsonGetString(json, "device_name",  tmp, sizeof(tmp))) strncpy(cfg_device_name,  tmp, sizeof(cfg_device_name)-1);
    if (wcJsonGetString(json, "api_base_url", tmp, sizeof(tmp))) strncpy(cfg_api_base_url, tmp, sizeof(cfg_api_base_url)-1);
    if (wcJsonGetString(json, "timezone",     tmp, sizeof(tmp))) strncpy(cfg_timezone,     tmp, sizeof(cfg_timezone)-1);
    configSave();
    wcServer.send(200, "application/json", "{\"ok\":true}");
}

static void handleGetPrompt() {
    static char escaped[8192];
    static char buf[8300];
    jsonEscapeBuf(escaped, sizeof(escaped), cfg_system_prompt);
    snprintf(buf, sizeof(buf), "{\"prompt\":\"%s\"}", escaped);
    wcServer.send(200, "application/json", buf);
}

static void handlePostPrompt() {
    if (!wcServer.hasArg("plain")) {
        wcServer.send(400, "application/json", "{\"error\":\"no body\"}"); return;
    }
    String body = wcServer.arg("plain");
    char prompt[4096] = "";
    wcJsonGetString(body.c_str(), "prompt", prompt, sizeof(prompt));
    strncpy(cfg_system_prompt, prompt, sizeof(cfg_system_prompt)-1);
    File f = LittleFS.open("/system_prompt.txt", "w");
    if (f) { f.print(cfg_system_prompt); f.close(); }
    wcServer.send(200, "application/json", "{\"ok\":true}");
}

static void handleGetMemory() {
    static char content[600], escaped[700], buf[800];
    wcReadFile("/memory.txt", content, sizeof(content));
    jsonEscapeBuf(escaped, sizeof(escaped), content);
    snprintf(buf, sizeof(buf), "{\"memory\":\"%s\"}", escaped);
    wcServer.send(200, "application/json", buf);
}

static void handlePostMemory() {
    if (!wcServer.hasArg("plain")) {
        wcServer.send(400, "application/json", "{\"error\":\"no body\"}"); return;
    }
    String body = wcServer.arg("plain");
    char content[512] = "";
    wcJsonGetString(body.c_str(), "memory", content, sizeof(content));
    File f = LittleFS.open("/memory.txt", "w");
    if (f) { f.print(content); f.close(); }
    wcServer.send(200, "application/json", "{\"ok\":true}");
}

static void handleGetStatus() {
    static char buf[512];
    int rule_count = 0, dev_count = 0;
    const Rule *rules = ruleGetAll();
    Device *devs = deviceGetAll();
    for (int i = 0; i < MAX_RULES;   i++) if (rules[i].used) rule_count++;
    for (int i = 0; i < MAX_DEVICES; i++) if (devs[i].used)  dev_count++;
    snprintf(buf, sizeof(buf),
        "{\"version\":\"%s\",\"uptime\":%lu,\"free_heap\":%u,"
        "\"ip\":\"%s\",\"rules\":%d,\"devices\":%d,\"chip\":\"ESP32\"}",
        WIRECLAW_VERSION, millis()/1000, ESP.getFreeHeap(),
        WiFi.localIP().toString().c_str(), rule_count, dev_count);
    wcServer.send(200, "application/json", buf);
}

static void handleReboot() {
    wcServer.send(200, "application/json", "{\"ok\":true}");
    delay(500); ESP.restart();
}

static void handleNotFound() {
    wcServer.send(404, "text/plain", "Not found");
}

static const char WC_HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WireClaw</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Courier New',monospace;background:#0a0a0a;color:#e0e0e0;padding:16px;max-width:520px;margin:0 auto}
h1{color:#00d4aa;font-size:1.3em;margin-bottom:2px}.sub{color:#555;font-size:.8em;margin-bottom:16px}
.tab{display:inline-block;padding:6px 14px;cursor:pointer;border:1px solid #333;color:#888;font-size:.85em;margin-right:4px;border-radius:3px}
.tab.active{border-color:#00d4aa;color:#00d4aa}.panel{display:none;margin-top:14px}.panel.active{display:block}
label{display:block;margin:10px 0 3px;color:#00d4aa;font-size:.85em}
input,textarea{width:100%;padding:8px;background:#111;border:1px solid #333;color:#fff;font-family:inherit;font-size:.9em;border-radius:3px}
input:focus,textarea:focus{outline:none;border-color:#00d4aa}textarea{resize:vertical;min-height:80px}
button{padding:8px 18px;background:#00d4aa;color:#0a0a0a;border:none;cursor:pointer;font-family:inherit;font-size:.9em;font-weight:bold;border-radius:3px;margin-top:10px}
button.sec{background:#333;color:#ccc;margin-left:6px}button.danger{background:#c0392b;color:#fff}
.msg{margin-top:8px;font-size:.8em;color:#00d4aa;min-height:16px}.stat{color:#888;font-size:.8em;line-height:1.7}.stat span{color:#ccc}
hr{border:none;border-top:1px solid #222;margin:12px 0}
</style></head><body>
<h1>&gt; WireClaw</h1><p class="sub" id="ver">ESP32 Classic</p>
<div>
  <span class="tab active" onclick="show(this,'cfg')">Config</span>
  <span class="tab" onclick="show(this,'prompt')">Prompt</span>
  <span class="tab" onclick="show(this,'memory')">Memory</span>
  <span class="tab" onclick="show(this,'status')">Status</span>
</div>
<div id="cfg" class="panel active">
  <label>WiFi SSID</label><input id="wifi_ssid">
  <label>WiFi Password</label><input id="wifi_pass" type="password" placeholder="blank = keep current">
  <label>API Key</label><input id="api_key" placeholder="blank = keep current">
  <label>Model</label><input id="model">
  <label>Device Name</label><input id="device_name">
  <label>API Base URL</label><input id="api_base_url" placeholder="http://... for local LLM">
  <label>Timezone</label><input id="timezone" placeholder="UTC0">
  <button onclick="saveConfig()">Save</button>
  <button class="danger" onclick="reboot()">Reboot</button>
  <p class="msg" id="cfg_msg"></p>
</div>
<div id="prompt" class="panel">
  <label>System Prompt</label><textarea id="sys_prompt" rows="12"></textarea>
  <button onclick="savePrompt()">Save</button><p class="msg" id="prompt_msg"></p>
</div>
<div id="memory" class="panel">
  <label>Memory (/memory.txt)</label><textarea id="mem_content" rows="8"></textarea>
  <button onclick="saveMemory()">Save</button><p class="msg" id="mem_msg"></p>
</div>
<div id="status" class="panel">
  <div class="stat" id="stat_content">Loading...</div>
  <button class="sec" onclick="loadStatus()">Refresh</button>
</div>
<script>
function show(el,id){document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));document.getElementById(id).classList.add('active');el.classList.add('active');if(id==='status')loadStatus();}
async function api(m,p,b){const o={method:m,headers:{'Content-Type':'application/json'}};if(b)o.body=JSON.stringify(b);return(await fetch(p,o)).json();}
async function loadConfig(){const d=await api('GET','/api/config');['wifi_ssid','model','device_name','api_base_url','timezone'].forEach(k=>{const el=document.getElementById(k);if(el)el.value=d[k]||'';});document.getElementById('ver').textContent='WireClaw '+d.device_name+' (ESP32 Classic)';}
async function saveConfig(){const d=await api('POST','/api/config',{wifi_ssid:document.getElementById('wifi_ssid').value,wifi_pass:document.getElementById('wifi_pass').value,api_key:document.getElementById('api_key').value,model:document.getElementById('model').value,device_name:document.getElementById('device_name').value,api_base_url:document.getElementById('api_base_url').value,timezone:document.getElementById('timezone').value});document.getElementById('cfg_msg').textContent=d.ok?'Saved!':'Error';}
async function loadPrompt(){const d=await api('GET','/api/prompt');document.getElementById('sys_prompt').value=d.prompt||'';}
async function savePrompt(){const d=await api('POST','/api/prompt',{prompt:document.getElementById('sys_prompt').value});document.getElementById('prompt_msg').textContent=d.ok?'Saved!':'Error';}
async function loadMemory(){const d=await api('GET','/api/memory');document.getElementById('mem_content').value=d.memory||'';}
async function saveMemory(){const d=await api('POST','/api/memory',{memory:document.getElementById('mem_content').value});document.getElementById('mem_msg').textContent=d.ok?'Saved!':'Error';}
async function loadStatus(){const d=await api('GET','/api/status');document.getElementById('stat_content').innerHTML='<b>Device</b><hr>Version: <span>'+d.version+'</span><br>Uptime: <span>'+d.uptime+'s</span><br>Free heap: <span>'+d.free_heap+' B</span><br>IP: <span>'+d.ip+'</span><hr>Rules: <span>'+d.rules+'</span><br>Devices: <span>'+d.devices+'</span>';}
async function reboot(){if(!confirm('Reboot?'))return;await api('POST','/api/reboot');}
loadConfig();loadPrompt();loadMemory();
</script></body></html>)rawhtml";

void webConfigInit() {
    wcServer.on("/",           HTTP_GET,  [](){wcServer.send_P(200,"text/html",WC_HTML);});
    wcServer.on("/api/config", HTTP_GET,  handleGetConfig);
    wcServer.on("/api/config", HTTP_POST, handlePostConfig);
    wcServer.on("/api/prompt", HTTP_GET,  handleGetPrompt);
    wcServer.on("/api/prompt", HTTP_POST, handlePostPrompt);
    wcServer.on("/api/memory", HTTP_GET,  handleGetMemory);
    wcServer.on("/api/memory", HTTP_POST, handlePostMemory);
    wcServer.on("/api/status", HTTP_GET,  handleGetStatus);
    wcServer.on("/api/reboot", HTTP_POST, handleReboot);
    wcServer.onNotFound(handleNotFound);
    wcServer.begin();
    if (MDNS.begin(cfg_device_name)) {
        MDNS.addService("http","tcp",80);
        Serial.printf("mDNS: http://%s.local/\n", cfg_device_name);
    }
    Serial.printf("Web config: http://%s/\n", WiFi.localIP().toString().c_str());
}

void webConfigHandle() { wcServer.handleClient(); }
