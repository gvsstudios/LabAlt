//0byte@gvs

#include "config.h"
#include <LittleFS.h>

static Device g_devices[MAX_DEVICES];

// ============================================================
// Kind helpers
// ============================================================

bool deviceIsSensor(DeviceKind kind) {
    return kind <= DEV_SENSOR_NATS_VALUE;
}

bool deviceIsActuator(DeviceKind kind) {
    return kind >= DEV_ACTUATOR_DIGITAL;
}

const char *deviceKindName(DeviceKind kind) {
    switch (kind) {
        case DEV_SENSOR_DIGITAL:       return "digital_in";
        case DEV_SENSOR_ANALOG_RAW:    return "analog_in";
        case DEV_SENSOR_NTC_10K:       return "ntc_10k";
        case DEV_SENSOR_LDR:           return "ldr";
        case DEV_SENSOR_INTERNAL_TEMP: return "internal_temp";
        case DEV_SENSOR_CLOCK_HOUR:    return "clock_hour";
        case DEV_SENSOR_CLOCK_MINUTE:  return "clock_minute";
        case DEV_SENSOR_CLOCK_HHMM:    return "clock_hhmm";
        case DEV_SENSOR_NATS_VALUE:    return "nats_value";
        case DEV_ACTUATOR_DIGITAL:     return "digital_out";
        case DEV_ACTUATOR_RELAY:       return "relay";
        case DEV_ACTUATOR_PWM:         return "pwm";
        case DEV_ACTUATOR_RGB_LED:     return "rgb_led";
        default:                       return "unknown";
    }
}

static DeviceKind kindFromString(const char *s) {
    if (strcmp(s, "digital_in") == 0)    return DEV_SENSOR_DIGITAL;
    if (strcmp(s, "analog_in") == 0)     return DEV_SENSOR_ANALOG_RAW;
    if (strcmp(s, "ntc_10k") == 0)       return DEV_SENSOR_NTC_10K;
    if (strcmp(s, "ldr") == 0)           return DEV_SENSOR_LDR;
    if (strcmp(s, "internal_temp") == 0) return DEV_SENSOR_INTERNAL_TEMP;
    if (strcmp(s, "clock_hour") == 0)    return DEV_SENSOR_CLOCK_HOUR;
    if (strcmp(s, "clock_minute") == 0)  return DEV_SENSOR_CLOCK_MINUTE;
    if (strcmp(s, "clock_hhmm") == 0)    return DEV_SENSOR_CLOCK_HHMM;
    if (strcmp(s, "nats_value") == 0)    return DEV_SENSOR_NATS_VALUE;
    if (strcmp(s, "digital_out") == 0)   return DEV_ACTUATOR_DIGITAL;
    if (strcmp(s, "relay") == 0)         return DEV_ACTUATOR_RELAY;
    if (strcmp(s, "pwm") == 0)           return DEV_ACTUATOR_PWM;
    if (strcmp(s, "rgb_led") == 0)       return DEV_ACTUATOR_RGB_LED;
    return DEV_SENSOR_DIGITAL;
}

// ============================================================
// CRUD
// ============================================================

Device *deviceGetAll()        { return g_devices; }
Device *deviceGetAllMutable() { return g_devices; }

Device *deviceFind(const char *name) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_devices[i].used && strcmp(g_devices[i].name, name) == 0)
            return &g_devices[i];
    }
    return NULL;
}

bool deviceRegister(const char *name, DeviceKind kind, uint8_t pin,
                    const char *unit, bool inverted,
                    const char *nats_subject, uint32_t baud) {
    if (deviceFind(name)) return false;

    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!g_devices[i].used) {
            strncpy(g_devices[i].name, name, DEV_NAME_LEN - 1);
            g_devices[i].name[DEV_NAME_LEN - 1] = '\0';
            g_devices[i].kind = kind;
            g_devices[i].pin  = pin;
            if (unit) {
                strncpy(g_devices[i].unit, unit, DEV_UNIT_LEN - 1);
                g_devices[i].unit[DEV_UNIT_LEN - 1] = '\0';
            } else {
                g_devices[i].unit[0] = '\0';
            }
            g_devices[i].inverted = inverted;
            g_devices[i].used     = true;
            g_devices[i].nats_value = 0.0f;
            g_devices[i].nats_msg[0] = '\0';
            g_devices[i].nats_sid = 0;
            g_devices[i].baud = baud;

            if (nats_subject && nats_subject[0]) {
                strncpy(g_devices[i].nats_subject, nats_subject,
                        sizeof(g_devices[i].nats_subject) - 1);
            } else {
                g_devices[i].nats_subject[0] = '\0';
            }

            // Set pin mode based on kind
            if (deviceIsSensor(kind)) {
                if (kind == DEV_SENSOR_ANALOG_RAW || kind == DEV_SENSOR_NTC_10K || kind == DEV_SENSOR_LDR) {
                    // ADC pins don't need pinMode
                } else {
                    pinMode(pin, INPUT_PULLUP);
                }
            } else if (deviceIsActuator(kind)) {
                if (kind == DEV_ACTUATOR_PWM) {
                    // PWM setup handled by espPWMWrite
                } else {
                    pinMode(pin, OUTPUT);
                }
            }
            return true;
        }
    }
    return false;
}

bool deviceRemove(const char *name) {
    Device *dev = deviceFind(name);
    if (!dev) return false;
    dev->used = false;
    dev->name[0] = '\0';
    return true;
}

// ============================================================
// NTC thermistor (ESP32 ADC: 12-bit, 0-4095, 3.3V reference)
// ============================================================
static void ntcReadWithWarmup(Device *dev) {
    int32_t sum = 0;
    for (int s = 0; s < 4; s++) sum += espAnalogRead(dev->pin);
    float raw = sum / 4.0f;
    float mV  = raw * 3300.0f / 4095.0f;

    if (mV <= 0 || mV >= 3290) { dev->ema = -999.0f; dev->ema_init = true; return; }
    float ratio = mV / 3300.0f;
    float resistance = dev->inverted
        ? 10000.0f * (1.0f - ratio) / ratio
        : 10000.0f * ratio / (1.0f - ratio);
    float tempK = 1.0f / (1.0f / 298.15f + (1.0f / 3950.0f) * log(resistance / 10000.0f));
    dev->ema = tempK - 273.15f;
    dev->ema_init = true;
}

// ============================================================
// Sensor reading (ESP32 direct GPIO)
// ============================================================
float deviceReadSensor(Device *dev, bool record_hist) {
    if (!dev || !dev->used) return 0.0f;

    float result = 0.0f;
    bool record_history = false;

    switch (dev->kind) {
        case DEV_SENSOR_DIGITAL:
            result = (float)espDigitalRead(dev->pin);
            break;

        case DEV_SENSOR_ANALOG_RAW:
            result = (float)espAnalogRead(dev->pin);
            record_history = true;
            break;

        case DEV_SENSOR_NTC_10K:
            if (!dev->ema_init) ntcReadWithWarmup(dev);
            result = dev->ema;
            record_history = true;
            break;

        case DEV_SENSOR_LDR: {
            int32_t sum = 0;
            for (int s = 0; s < 4; s++) sum += espAnalogRead(dev->pin);
            float raw = sum / 4.0f;
            float mV  = raw * 3300.0f / 4095.0f;
            result = mV * 100.0f / 3300.0f;
            record_history = true;
            break;
        }

        case DEV_SENSOR_INTERNAL_TEMP:
            result = espInternalTemp();
            break;

        case DEV_SENSOR_CLOCK_HOUR: {
            struct tm timeinfo;
            result = getLocalTime(&timeinfo, 0) ? (float)timeinfo.tm_hour : -1.0f;
            break;
        }

        case DEV_SENSOR_CLOCK_MINUTE: {
            struct tm timeinfo;
            result = getLocalTime(&timeinfo, 0) ? (float)timeinfo.tm_min : -1.0f;
            break;
        }

        case DEV_SENSOR_CLOCK_HHMM: {
            struct tm timeinfo;
            result = getLocalTime(&timeinfo, 0)
                     ? (float)(timeinfo.tm_hour * 100 + timeinfo.tm_min)
                     : -1.0f;
            break;
        }

        case DEV_SENSOR_NATS_VALUE:
            result = dev->nats_value;
            break;

        default:
            break;
    }

    if (record_history && record_hist) {
        dev->history[dev->history_idx] = result;
        dev->history_idx = (dev->history_idx + 1) % DEV_HISTORY_LEN;
        if (!dev->history_full && dev->history_idx == 0) dev->history_full = true;
    }

    return result;
}

// ============================================================
// Actuator control (ESP32 direct GPIO)
// ============================================================
bool deviceSetActuator(Device *dev, int value) {
    if (!dev || !dev->used || !deviceIsActuator(dev->kind)) return false;

    dev->last_value = value;

    switch (dev->kind) {
        case DEV_ACTUATOR_DIGITAL:
            if (dev->pin == PIN_NONE) return false;
            return espDigitalWrite(dev->pin, value ? 1 : 0);

        case DEV_ACTUATOR_RELAY: {
            if (dev->pin == PIN_NONE) return false;
            int final_val = dev->inverted ? (value ? 0 : 1) : (value ? 1 : 0);
            return espDigitalWrite(dev->pin, final_val);
        }

        case DEV_ACTUATOR_PWM:
            if (dev->pin == PIN_NONE) return false;
            return espPWMWrite(dev->pin, constrain(value, 0, 255));

        case DEV_ACTUATOR_RGB_LED:
            pinMode(LED_BUILTIN, OUTPUT);
            digitalWrite(LED_BUILTIN, value ? HIGH : LOW);
            g_led_user = (value != 0);
            return true;

        default:
            return false;
    }
}

// ============================================================
// NATS helpers (stubs - no NATS in this build)
// ============================================================
void deviceSetNatsValue(Device *dev, float value, const char *msg) {
    if (!dev) return;
    dev->nats_value = value;
    if (msg) {
        strncpy(dev->nats_msg, msg, sizeof(dev->nats_msg) - 1);
        dev->nats_msg[sizeof(dev->nats_msg) - 1] = '\0';
    } else {
        dev->nats_msg[0] = '\0';
    }
}

const char *deviceGetNatsMsg(const Device *dev) {
    if (!dev || dev->kind != DEV_SENSOR_NATS_VALUE) return "";
    return dev->nats_msg;
}

void parseNatsPayload(const uint8_t *data, size_t len,
                      float *out_value, char *out_msg, size_t msg_len) {
    *out_value = 0.0f;
    if (msg_len > 0) out_msg[0] = '\0';
    (void)data; (void)len;
}

// ============================================================
// JSON persistence helpers
// ============================================================
static bool devJsonGetString(const char *json, const char *key,
                              char *dst, int dst_len) {
    char pattern[48];
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

static int devJsonGetInt(const char *json, const char *key, int default_val) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    return atoi(p);
}

static bool devJsonGetBool(const char *json, const char *key, bool default_val) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return default_val;
}

void devicesSave() {
    static char buf[2048];
    int w = 0;
    w += snprintf(buf + w, sizeof(buf) - w, "[");

    bool first = true;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!g_devices[i].used) continue;
        const Device *d = &g_devices[i];
        if (!first) w += snprintf(buf + w, sizeof(buf) - w, ",");
        first = false;

        w += snprintf(buf + w, sizeof(buf) - w,
            "{\"n\":\"%s\",\"k\":\"%s\",\"p\":%d,\"u\":\"%s\",\"i\":%s",
            d->name, deviceKindName(d->kind), d->pin,
            d->unit, d->inverted ? "true" : "false");
        if (d->nats_subject[0])
            w += snprintf(buf + w, sizeof(buf) - w,
                ",\"ns\":\"%s\"", d->nats_subject);
        w += snprintf(buf + w, sizeof(buf) - w, "}");
        if (w >= (int)sizeof(buf) - 1) break;
    }

    w += snprintf(buf + w, sizeof(buf) - w, "]");
    File f = LittleFS.open("/devices.json", "w");
    if (f) { f.print(buf); f.close(); }
    if (g_debug) Serial.printf("Devices: saved (%d bytes)\n", w);
}

static void devicesLoad() {
    static char buf[2048];
    File f = LittleFS.open("/devices.json", "r");
    if (!f) return;
    int len = f.readBytes(buf, sizeof(buf) - 1);
    buf[len] = '\0';
    f.close();
    if (len <= 2) return;

    const char *p = buf;
    int count = 0;

    while (*p && count < MAX_DEVICES) {
        const char *obj = strchr(p, '{');
        if (!obj) break;
        const char *obj_end = strchr(obj, '}');
        if (!obj_end) break;

        int obj_len = obj_end - obj + 1;
        static char objBuf[256];
        if (obj_len >= (int)sizeof(objBuf)) { p = obj_end + 1; continue; }
        memcpy(objBuf, obj, obj_len); objBuf[obj_len] = '\0';

        char name[DEV_NAME_LEN];
        char kind_str[24];
        char unit[DEV_UNIT_LEN];

        if (!devJsonGetString(objBuf, "n", name, sizeof(name)))
            { p = obj_end + 1; continue; }
        if (!devJsonGetString(objBuf, "k", kind_str, sizeof(kind_str)))
            { p = obj_end + 1; continue; }

        if (strcmp(kind_str, "serial_text") == 0) { p = obj_end + 1; continue; }

        int pin = devJsonGetInt(objBuf, "p", PIN_NONE);
        devJsonGetString(objBuf, "u", unit, sizeof(unit));
        bool inverted = devJsonGetBool(objBuf, "i", false);

        char nats_subj[32] = "";
        devJsonGetString(objBuf, "ns", nats_subj, sizeof(nats_subj));

        DeviceKind kind = kindFromString(kind_str);
        deviceRegister(name, kind, (uint8_t)pin, unit, inverted,
                       nats_subj[0] ? nats_subj : NULL, 0);
        count++;
        p = obj_end + 1;
    }

    Serial.printf("Devices: loaded %d from /devices.json\n", count);
}

void devicesClear() {
    memset(g_devices, 0, sizeof(g_devices));
}

void devicesReload() {
    devicesClear();
    devicesLoad();
    bool changed = false;
    if (!deviceFind("clock_hour"))   { deviceRegister("clock_hour", DEV_SENSOR_CLOCK_HOUR, PIN_NONE, "h", false, NULL, 0); changed = true; }
    if (!deviceFind("clock_minute")) { deviceRegister("clock_minute", DEV_SENSOR_CLOCK_MINUTE, PIN_NONE, "m", false, NULL, 0); changed = true; }
    if (!deviceFind("clock_hhmm"))   { deviceRegister("clock_hhmm", DEV_SENSOR_CLOCK_HHMM, PIN_NONE, "", false, NULL, 0); changed = true; }
    // Fan now on ESP32 GPIO25
    if (!deviceFind("fan"))          { deviceRegister("fan", DEV_ACTUATOR_PWM, FAN_PIN, "%", false, NULL, 0); changed = true; }
    if (changed) devicesSave();
    int count = 0;
    for (int i = 0; i < MAX_DEVICES; i++) if (g_devices[i].used) count++;
    Serial.printf("Devices: reloaded (%d registered)\n", count);
}

// ============================================================
// Background sensor polling (NTC warmup every 5s)
// ============================================================
void sensorsPoll() {
    static uint32_t last_ntc  = 0;
    static uint32_t last_hist = 0;
    uint32_t now = millis();

    bool do_ntc  = (now - last_ntc  >= 5000);
    bool do_hist = (now - last_hist >= 300000);
    if (!do_ntc && !do_hist) return;
    if (do_ntc)  last_ntc  = now;
    if (do_hist) last_hist = now;

    for (int i = 0; i < MAX_DEVICES; i++) {
        Device *d = &g_devices[i];
        if (!d->used || !deviceIsSensor(d->kind)) continue;
        if (d->kind == DEV_SENSOR_NTC_10K && do_ntc) ntcReadWithWarmup(d);
        if (do_hist) deviceReadSensor(d, true);
    }
}

// ============================================================
// Init
// ============================================================
void devicesInit() {
    memset(g_devices, 0, sizeof(g_devices));
    devicesLoad();

    bool changed = false;
    if (!deviceFind("chip_temp"))   { deviceRegister("chip_temp", DEV_SENSOR_INTERNAL_TEMP, PIN_NONE, "C", false, NULL, 0); changed = true; }
    if (!deviceFind("clock_hour"))  { deviceRegister("clock_hour", DEV_SENSOR_CLOCK_HOUR, PIN_NONE, "h", false, NULL, 0); changed = true; }
    if (!deviceFind("clock_minute")){ deviceRegister("clock_minute", DEV_SENSOR_CLOCK_MINUTE, PIN_NONE, "m", false, NULL, 0); changed = true; }
    if (!deviceFind("clock_hhmm"))  { deviceRegister("clock_hhmm", DEV_SENSOR_CLOCK_HHMM, PIN_NONE, "", false, NULL, 0); changed = true; }
    if (!deviceFind("fan"))         { deviceRegister("fan", DEV_ACTUATOR_PWM, FAN_PIN, "%", false, NULL, 0); changed = true; }
    if (changed) devicesSave();

    int count = 0;
    for (int i = 0; i < MAX_DEVICES; i++) if (g_devices[i].used) count++;
    Serial.printf("Devices: %d registered\n", count);
}