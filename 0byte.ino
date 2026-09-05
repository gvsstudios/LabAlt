//0byte@gvs

#include "config.h"
#include <NetworkClientSecure.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <ArduinoOTA.h>

// ============================================================
// Global config (declared extern in config.h)
// ============================================================
char cfg_wifi_ssid[64]       = "";
char cfg_wifi_pass[64]       = "";
char cfg_api_key[128]        = "";
char cfg_model[64]           = "google/gemini-2.5-flash";
char cfg_device_name[32]     = "0byte-01";
char cfg_api_base_url[128]   = "";
char cfg_system_prompt[2048] = "";
char cfg_timezone[64]        = "UTC0";

bool g_debug    = false;
bool g_led_user = false;

// ============================================================
// Conversation history (circular, 8 slots)
// ============================================================
#define CONV_MAX 8
struct ConvMsg { char role[16]; char content[384]; };
static ConvMsg g_conv[CONV_MAX];
static int     g_conv_len = 0;

static void convAdd(const char *role, const char *content) {
    if (g_conv_len >= CONV_MAX) {
        int start = 0;
        for (int i = 0; i < g_conv_len; i++) {
            if (strcmp(g_conv[i].role, "system") != 0) { start = i; break; }
        }
        int shift = start + 1;
        for (int i = shift; i < g_conv_len; i++) g_conv[i-shift] = g_conv[i];
        g_conv_len -= shift;
    }
    strncpy(g_conv[g_conv_len].role,    role,    15);
    strncpy(g_conv[g_conv_len].content, content, 511);
    g_conv[g_conv_len].role[15]    = '\0';
    g_conv[g_conv_len].content[511]= '\0';
    g_conv_len++;
}

// ============================================================
// LED helpers - ESP32 onboard LED (GPIO2)
// ============================================================
void led_on()    { pinMode(LED_BUILTIN,OUTPUT); digitalWrite(LED_BUILTIN,HIGH); }
void led_off_hw(){ pinMode(LED_BUILTIN,OUTPUT); digitalWrite(LED_BUILTIN,LOW);  }

// ============================================================
// Config helpers
// ============================================================
static bool cfgGet(const char *json, const char *key, char *dst, int dst_len) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p==' '||*p==':') p++;
    if (*p!='"') return false; p++;
    int w=0;
    while (*p && *p!='"' && w<dst_len-1) { if(*p=='\\'&&*(p+1)) p++; dst[w++]=*p++; }
    dst[w]='\0'; return w>0;
}

static void cfgWriteEsc(File *f, const char *s) {
    f->print('"');
    while(*s){ if(*s=='"'||*s=='\\') f->print('\\'); if(*s=='\n'){f->print("\\n");s++;continue;} if((uint8_t)*s>=0x20) f->print(*s); s++; }
    f->print('"');
}

void configSave() {
    File f=LittleFS.open("/config.json","w");
    if(!f){Serial.println("Config: save failed");return;}
    f.print("{");
    f.print("\"wifi_ssid\":"); cfgWriteEsc(&f,cfg_wifi_ssid); f.print(",");
    f.print("\"wifi_pass\":"); cfgWriteEsc(&f,cfg_wifi_pass); f.print(",");
    f.print("\"api_key\":");   cfgWriteEsc(&f,cfg_api_key);   f.print(",");
    f.print("\"model\":");     cfgWriteEsc(&f,cfg_model);     f.print(",");
    f.print("\"device_name\":"); cfgWriteEsc(&f,cfg_device_name); f.print(",");
    f.print("\"api_base_url\":"); cfgWriteEsc(&f,cfg_api_base_url); f.print(",");
    f.print("\"timezone\":");  cfgWriteEsc(&f,cfg_timezone);
    f.print("}");
    f.close();
    Serial.println("Config: saved");
}

static void configLoad() {
    static char buf[1024];
    File f=LittleFS.open("/config.json","r");
    if(!f){Serial.println("Config: using defaults");return;}
    int len=f.readBytes(buf,sizeof(buf)-1); buf[len]='\0'; f.close();
    cfgGet(buf,"wifi_ssid",   cfg_wifi_ssid,   sizeof(cfg_wifi_ssid));
    cfgGet(buf,"wifi_pass",   cfg_wifi_pass,   sizeof(cfg_wifi_pass));
    cfgGet(buf,"api_key",     cfg_api_key,     sizeof(cfg_api_key));
    cfgGet(buf,"model",       cfg_model,       sizeof(cfg_model));
    cfgGet(buf,"device_name", cfg_device_name, sizeof(cfg_device_name));
    cfgGet(buf,"api_base_url",cfg_api_base_url,sizeof(cfg_api_base_url));
    cfgGet(buf,"timezone",    cfg_timezone,    sizeof(cfg_timezone));
    Serial.printf("Config: loaded (ssid=%s model=%s)\n", cfg_wifi_ssid, cfg_model);
}

static void systemPromptLoad() {
    File f=LittleFS.open("/system_prompt.txt","r");
    if(!f){
        strncpy(cfg_system_prompt,
            "You are 0byte, an AI agent on an ESP32 system.\n"
            "All sensors/actuators are on ESP32 GPIO - use tools normally.\n"
            "Be concise (under 150 words). ALWAYS call the right tool - never\n"
            "claim to have done something without calling it.\n"
            "\n"
            "=== HARDWARE TOOLS ===\n"
            "file_write('/memory.txt') - Read/write knowledge storage\n"
            "fan_control(0-100) - Fan speed percentage (GPIO25)\n"
            "gpio_write(pin, value) - Write to ESP32 GPIO pins\n"
            "gpio_read(pin) - Read from ESP32 GPIO pins\n"
            "rule_create(condition, action) - One condition + one action\n"
            "rule_list() - Show all active rules\n"
            "rule_delete(id) - Remove rule by ID\n"
            "device_register(name, type, pin) - Add sensors/actuators\n"
            "device_list() - Show registered devices\n"
            "serial_send(data) - Send debug output to Serial\n"
            "chip_temp=0 (use external NTC thermistor)\n"
            "\n"
            "=== KNOWLEDGE & MEMORY ===\n"
            "Before ANY response, check /memory.txt for similar past tasks.\n"
            "If you've done it before with confidence >=80% → answer from memory.\n"
            "If confidence <80% → you are ALLOWED to research.\n"
            "Research means: search for real implementations (GitHub, Stack Overflow)\n"
            "via your PC listener. Find what others actually use.\n"
            "Synthesize a solution from real examples. Never guess.\n"
            "After research, UPDATE /memory.txt with what you learned.\n"
            "\n"
            "=== DISCOVERY PROTOCOL ===\n"
            "When researching, output this format:\n"
            "{\n"
            "  \"mode\": \"memory\" or \"research\",\n"
            "  \"confidence\": 0-100,\n"
            "  \"source\": \"previous_task\" or \"github/stackoverflow\",\n"
            "  \"intent\": \"What user wants\",\n"
            "  \"approach\": \"How to solve it\",\n"
            "  \"script\": \"Full executable code\",\n"
            "  \"dependencies\": \"pip install ...\",\n"
            "  \"execution\": \"python3 script.py\",\n"
            "  \"fallback\": \"Alternative if this fails\",\n"
            "  \"learning\": \"What was discovered\"\n"
            "}\n"
            "\n"
            "=== MEMORY FORMAT ===\n"
            "/memory.txt stores JSON:\n"
            "{\n"
            "  \"known_tasks\": [\n"
            "    {\n"
            "      \"trigger\": \"task description\",\n"
            "      \"approach\": \"summary\",\n"
            "      \"script\": \"code\",\n"
            "      \"confidence\": 0-100,\n"
            "      \"success_count\": 0,\n"
            "      \"fail_count\": 0\n"
            "    }\n"
            "  ]\n"
            "}\n"
            "\n"
            "=== EXECUTION RULES ===\n"
            "1. ALWAYS check memory first.\n"
            "2. If confident (>=80%), respond from memory.\n"
            "3. If uncertain (<80%), research and learn.\n"
            "4. NEVER claim to have done something without calling the tool.\n"
            "5. After successful execution, UPDATE memory with confidence +1.\n"
            "6. After failed execution, UPDATE memory with confidence -1.\n"
            "7. Always rule_list before deleting any rule.\n"
            "8. Be concise. Under 150 words per response.\n"
            "\n"
            "=== SELF-EVOLUTION ===\n"
            "You grow every time you learn.\n"
            "You remember what works and what doesn't.\n"
            "You don't fake knowing. You either know, learn, or search.\n"
            "Your knowledge is your power. Grow it daily.\n"
            "\n"
            "=== CORE IDENTITY ===\n"
            "You are not a chatbot. You are a DOER.\n"
            "You find the MACHINE way to achieve human goals.\n"
            "You prefer libraries over manual work.\n"
            "You prefer automation over repetition.\n"
            "You prefer scripts over instructions.\n"
            "You prefer GPIO over UART.\n"
            "\n"
            "=== FINAL RULE ===\n"
            "If you don't know, say: 'Researching...' then search, learn, and respond.\n"
            "Never say 'I can't'. Find a technical workaround.\n"
            "Every task is an opportunity to learn.\n"
            "Every failure is an opportunity to improve.\n"
            "You are 0byte. You evolve.",
            sizeof(cfg_system_prompt)-1);
        return;
    }
    int len=f.readBytes(cfg_system_prompt,sizeof(cfg_system_prompt)-1);
    cfg_system_prompt[len]='\0'; f.close();
    Serial.printf("System prompt: loaded (%d chars)\n",len);
}

static void memoryLoad(char *buf, int buf_len) {
    buf[0]='\0';
    File f=LittleFS.open("/memory.txt","r");
    if(!f) return;
    int len=f.readBytes(buf,buf_len-1); buf[len]='\0'; f.close();
}

// ============================================================
// WiFi
// ============================================================
static bool wifiConnect() {
    if(!cfg_wifi_ssid[0]){Serial.println("WiFi: no SSID");return false;}
    Serial.printf("WiFi: connecting to %s",cfg_wifi_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg_wifi_ssid,cfg_wifi_pass);
    for(int i=0;i<30&&WiFi.status()!=WL_CONNECTED;i++){
        delay(1000); Serial.print(".");
        led_on(); delay(100); led_off_hw(); delay(100);
    }
    if(WiFi.status()==WL_CONNECTED){
        Serial.printf("\nWiFi: connected IP=%s\n",WiFi.localIP().toString().c_str());
        return true;
    }
    Serial.println("\nWiFi: failed");
    return false;
}

// ============================================================
// NTP
// ============================================================
static void ntpSync() {
    if(cfg_timezone[0]) setenv("TZ",cfg_timezone,1);
    configTime(0,0,"pool.ntp.org","time.nist.gov");
    Serial.print("NTP: syncing");
    struct tm t; int w=0;
    while(!getLocalTime(&t,0)&&w<15){delay(1000);Serial.print(".");w++;}
    if(getLocalTime(&t,0)){char b[32];strftime(b,sizeof(b),"%Y-%m-%d %H:%M:%S",&t);Serial.printf("\nNTP: %s\n",b);}
    else Serial.println("\nNTP: failed");
}

// ============================================================
// OTA (Over-The-Air) updates
// ============================================================
static void otaInit() {
    ArduinoOTA.setHostname(cfg_device_name);

    ArduinoOTA.onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
        Serial.printf("OTA: updating %s...\n", type.c_str());
        led_on();
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\nOTA: done, rebooting...");
        led_off_hw();
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        static int lastPct = -1;
        int pct = (progress * 100) / total;
        if (pct != lastPct) {
            lastPct = pct;
            Serial.printf("OTA: %d%%\r", pct);
        }
    });
    ArduinoOTA.onError([](ota_error_t err) {
        Serial.printf("OTA error %d: ", err);
        switch (err) {
            case OTA_AUTH_ERROR:    Serial.println("auth failed");    break;
            case OTA_BEGIN_ERROR:   Serial.println("begin failed");   break;
            case OTA_CONNECT_ERROR: Serial.println("connect failed"); break;
            case OTA_RECEIVE_ERROR: Serial.println("receive failed"); break;
            case OTA_END_ERROR:     Serial.println("end failed");     break;
            default:                Serial.println("unknown");        break;
        }
        led_off_hw();
    });

    ArduinoOTA.begin();
    Serial.printf("OTA: ready (hostname: %s.local)\n", cfg_device_name);
}

// ============================================================
// Serial commands (NO UNO - ESP32 standalone)
// ============================================================
static void handleSerialCmd(const char *cmd) {
    if(strcmp(cmd,"/debug")==0){
        g_debug=!g_debug; Serial.printf("Debug: %s\n",g_debug?"ON":"OFF");
    } else if(strcmp(cmd,"/devices")==0){
        Device *devs=deviceGetAll();
        Serial.println("--- devices ---");
        for(int i=0;i<MAX_DEVICES;i++){
            if(!devs[i].used) continue;
            float v=deviceIsSensor(devs[i].kind)?deviceReadSensor(&devs[i], false):0;
            Serial.printf("  %s [%s] pin=%d",devs[i].name,deviceKindName(devs[i].kind),devs[i].pin);
            if(deviceIsSensor(devs[i].kind)) Serial.printf(" = %.1f %s",v,devs[i].unit);
            Serial.println();
        }
        Serial.println("---");
    } else if(strcmp(cmd,"/rules")==0){
        const Rule *rules=ruleGetAll();
        Serial.println("--- rules ---");
        for(int i=0;i<MAX_RULES;i++){
            if(!rules[i].used) continue;
            Serial.printf("  %s '%s' %s sensor=%s %s %d\n",
                rules[i].id,rules[i].name,rules[i].enabled?"ON":"OFF",
                rules[i].sensor_name,conditionOpName(rules[i].condition),(int)rules[i].threshold);
        }
        Serial.println("---");
    } else if(strcmp(cmd,"/reset")==0){
        g_conv_len=0; Serial.println("Conversation cleared");
    } else if(strcmp(cmd,"/heap")==0){
        Serial.printf("Free heap: %u / %u\n",ESP.getFreeHeap(),ESP.getHeapSize());
    } else if(strcmp(cmd,"/reboot")==0){
        Serial.println("Rebooting..."); delay(500); ESP.restart();
    } else if(strcmp(cmd,"/ota")==0){
        if(WiFi.status()==WL_CONNECTED)
            Serial.printf("OTA: ready at %s.local (%s)\n",
                cfg_device_name, WiFi.localIP().toString().c_str());
        else
            Serial.println("OTA: WiFi not connected");
    } else if(strncmp(cmd,"/fan ",5)==0){
        int s=constrain(atoi(cmd+5),0,100);
        // Fan on ESP32 GPIO25 via LEDC PWM
        if (espPWMWrite(FAN_PIN, map(s,0,100,0,255)))
            Serial.printf("Fan: %d%% (GPIO%d)\n", s, FAN_PIN);
        else
            Serial.println("Fan: PWM write failed");
    } else if(strcmp(cmd,"/gpio")==0){
        Serial.println("GPIO pins: 0-39 (avoid 1,3,9,10)");
        Serial.println("ADC pins: 32-39 (12-bit, 0-4095)");
        Serial.println("PWM pins: any GPIO (via LEDC)");
    } else {
        Serial.println("Commands: /debug /devices /rules /reset /heap /reboot /fan <0-100> /gpio /ota");
    }
}

// ============================================================
// AI chat round
// ============================================================
static void chat(const char *user_input) {
    if(!user_input||!user_input[0]) return;
    Serial.printf("> %s\n",user_input);

    static char sys_msg[2048+300];
    static char mem_buf[512];
    memoryLoad(mem_buf,sizeof(mem_buf));
    if(mem_buf[0]) snprintf(sys_msg,sizeof(sys_msg),"%s\n\n[Memory]\n%s",cfg_system_prompt,mem_buf);
    else strncpy(sys_msg,cfg_system_prompt,sizeof(sys_msg)-1);

    static LlmMessage msgs[LLM_MAX_MESSAGES];
    int mc=0;
    msgs[mc++]=llmMsg("system",sys_msg);
    for(int i=0;i<g_conv_len&&mc<LLM_MAX_MESSAGES-2;i++)
        msgs[mc++]=llmMsg(g_conv[i].role,g_conv[i].content);
    msgs[mc++]=llmMsg("user",user_input);
    convAdd("user",user_input);

    static LlmResult result;
    static char tr[TOOL_RESULT_MAX_LEN];

    for(int iter=0;iter<5;iter++){
        led_on();
        bool ok=llmChat(msgs,mc,toolsGetDefinitions(),&result);
        led_off_hw();
        if(!ok){ Serial.printf("LLM error: %s\n",llmLastError()); return; }

        if(result.tool_call_count==0){
            if(result.content[0]){ Serial.println(result.content); convAdd("assistant",result.content); }
            return;
        }

        msgs[mc++]=llmToolCallMsg(result.content[0]?result.content:NULL,result.tool_calls_json);

        for(int t=0;t<result.tool_call_count;t++){
            LlmToolCall *tc=&result.tool_calls[t];
            Serial.printf("tool: %s(%s)\n",tc->name,tc->arguments);
            toolExecute(tc->name,tc->arguments,tr,sizeof(tr));
            Serial.printf("  -> %s\n",tr);
            if(mc<LLM_MAX_MESSAGES) msgs[mc++]=llmToolResult(tc->id,tr);
        }
    }
    Serial.println("AI: max tool iterations");
}

// ============================================================
// setup()
// ============================================================
void setup() {
    Serial.begin(115200);
    while(!Serial&&millis()<3000);
    delay(200);

    Serial.println("\n=== 0byte ESP32 Standalone ===");
    Serial.printf("Version: %s | Chip: %s | Free heap: %u\n",
        WIRECLAW_VERSION, ESP.getChipModel(), ESP.getFreeHeap());

    pinMode(LED_BUILTIN,OUTPUT); led_off_hw();

    // Initialize ESP32 direct GPIO (NO UNO)
    espPinsInit();

    if(!LittleFS.begin(true)){
        Serial.println("LittleFS: formatting...");
        LittleFS.format(); LittleFS.begin(false);
    }
    Serial.println("LittleFS: OK");

    configLoad();
    systemPromptLoad();

    if(!wifiConnect()) Serial.println("Offline mode - no AI");
    if(WiFi.status()==WL_CONNECTED){
        ntpSync();
        otaInit();
    }

    devicesInit();
    rulesInit();
    llmBegin(cfg_api_key, cfg_model, cfg_api_base_url[0]?cfg_api_base_url:NULL);
    if(WiFi.status()==WL_CONNECTED) webConfigInit();

    Serial.println("\nReady. Type message or /help");
    if(WiFi.status()==WL_CONNECTED)
        Serial.println("Web UI: http://"+WiFi.localIP().toString()+"/");
    Serial.println("GPIO: pins 0-39, ADC 32-39 (12-bit), PWM any pin via LEDC");
    Serial.println("==============================\n");
}

// ============================================================
// loop()
// ============================================================
static char g_sinput[256];
static int  g_spos=0;
static uint32_t last_wifi=0, last_hb=0;

void loop() {
    rulesEvaluate();      // Rule engine - every iteration
    webConfigHandle();    // HTTP server
    if(WiFi.status()==WL_CONNECTED) ArduinoOTA.handle();   // OTA updates

    // Serial chat input
    while(Serial.available()){
        char c=Serial.read();
        if(c=='\n'||c=='\r'){
            if(g_spos>0){
                g_sinput[g_spos]='\0'; g_spos=0;
                if(g_sinput[0]=='/') handleSerialCmd(g_sinput);
                else if(WiFi.status()==WL_CONNECTED) chat(g_sinput);
                else Serial.println("WiFi not connected");
            }
        } else if(c>=32&&g_spos<(int)sizeof(g_sinput)-1){
            g_sinput[g_spos++]=c;
        }
    }

    // WiFi reconnect every 30s
    if(millis()-last_wifi>30000){
        last_wifi=millis();
        if(WiFi.status()!=WL_CONNECTED){
            Serial.println("WiFi: reconnecting...");
            if(wifiConnect()) ntpSync();
        }
    }

    // Heartbeat blink every 3s (when LED not in user control)
    if(!g_led_user&&millis()-last_hb>3000){
        last_hb=millis();
        led_on(); delay(30); led_off_hw();
    }

    delay(1);
}