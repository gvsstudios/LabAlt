//0byte@gvs

#include "config.h"
#include <LittleFS.h>

// ============================================================
// JSON argument parsers
// ============================================================

static int toolJsonArgInt(const char *json, const char *key, int default_val) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    return atoi(p);
}

static bool toolJsonArgString(const char *json, const char *key,
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

static bool toolJsonArgBool(const char *json, const char *key, bool default_val) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (strncmp(p, "true", 4) == 0)  return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return default_val;
}

static bool toolJsonArgExists(const char *json, const char *key) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern) != NULL;
}

// ============================================================
// Tool definitions JSON (OpenAI function calling format)
// ============================================================

static const char *TOOLS_JSON = R"JSON([
{"type":"function","function":{"name":"led_set","description":"Toggle built-in LED on/off (no RGB on classic DevKit). Use value>0 to turn on.","parameters":{"type":"object","properties":{"r":{"type":"integer"},"g":{"type":"integer"},"b":{"type":"integer"}},"required":["r","g","b"]}}},
{"type":"function","function":{"name":"gpio_write","description":"Set ESP32 GPIO pin HIGH/LOW","parameters":{"type":"object","properties":{"pin":{"type":"integer"},"value":{"type":"integer"}},"required":["pin","value"]}}},
{"type":"function","function":{"name":"gpio_read","description":"Read ESP32 GPIO pin state","parameters":{"type":"object","properties":{"pin":{"type":"integer"}},"required":["pin"]}}},
{"type":"function","function":{"name":"fan_control","description":"Control fan speed on GPIO25 via PWM. speed 0-100 percent.","parameters":{"type":"object","properties":{"speed":{"type":"integer","description":"Fan speed 0-100 percent"}},"required":["speed"]}}},
{"type":"function","function":{"name":"device_info","description":"Get heap, uptime, WiFi, chip info","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"file_read","description":"Read file from LittleFS filesystem","parameters":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}}},
{"type":"function","function":{"name":"file_write","description":"Write file to LittleFS filesystem","parameters":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]}}},
{"type":"function","function":{"name":"temperature_read","description":"Read chip temperature (returns 0 on classic ESP32 - use external sensor)","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"device_register","description":"Register sensor/actuator on ESP32 GPIO","parameters":{"type":"object","properties":{"name":{"type":"string"},"type":{"type":"string","enum":["digital_in","analog_in","ntc_10k","ldr","digital_out","relay","pwm"]},"pin":{"type":"integer","description":"ESP32 GPIO pin"},"unit":{"type":"string"},"inverted":{"type":"boolean"}},"required":["name","type"]}}},
{"type":"function","function":{"name":"device_list","description":"List registered devices","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"device_remove","description":"Remove device by name","parameters":{"type":"object","properties":{"name":{"type":"string"}},"required":["name"]}}},
{"type":"function","function":{"name":"sensor_read","description":"Read named sensor value","parameters":{"type":"object","properties":{"name":{"type":"string"}},"required":["name"]}}},
{"type":"function","function":{"name":"actuator_set","description":"Set actuator value","parameters":{"type":"object","properties":{"name":{"type":"string"},"value":{"type":"integer"}},"required":["name","value"]}}},
{"type":"function","function":{"name":"rule_create","description":"Create automation rule","parameters":{"type":"object","properties":{"rule_name":{"type":"string"},"sensor_name":{"type":"string"},"sensor_pin":{"type":"integer"},"condition":{"type":"string","description":"gt|lt|eq|neq|change|always|chained"},"threshold":{"type":"integer"},"interval_seconds":{"type":"integer"},"actuator_name":{"type":"string"},"on_action":{"type":"string","description":"gpio_write|led_set|actuator|serial_send"},"on_pin":{"type":"integer"},"on_value":{"type":"integer"},"on_r":{"type":"integer"},"on_g":{"type":"integer"},"on_b":{"type":"integer"},"on_serial_text":{"type":"string"},"off_action":{"type":"string","description":"auto|none|gpio_write|led_set|actuator|serial_send"},"off_pin":{"type":"integer"},"off_value":{"type":"integer"},"off_r":{"type":"integer"},"off_g":{"type":"integer"},"off_b":{"type":"integer"},"off_serial_text":{"type":"string"},"chain_rule":{"type":"string"},"chain_delay_seconds":{"type":"integer"},"chain_off_rule":{"type":"string"},"chain_off_delay_seconds":{"type":"integer"}},"required":["rule_name"]}}},
{"type":"function","function":{"name":"rule_list","description":"List all rules","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"rule_delete","description":"Delete rule by ID or 'all'","parameters":{"type":"object","properties":{"rule_id":{"type":"string"}},"required":["rule_id"]}}},
{"type":"function","function":{"name":"rule_enable","description":"Enable/disable rule","parameters":{"type":"object","properties":{"rule_id":{"type":"string"},"enabled":{"type":"boolean"}},"required":["rule_id","enabled"]}}},
{"type":"function","function":{"name":"serial_send","description":"Send text to Serial monitor (debug output)","parameters":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}}},
{"type":"function","function":{"name":"chain_create","description":"Create multi-step automation chain (up to 5 steps)","parameters":{"type":"object","properties":{"sensor_name":{"type":"string"},"condition":{"type":"string"},"threshold":{"type":"integer"},"interval_seconds":{"type":"integer"},"step1_action":{"type":"string"},"step1_message":{"type":"string"},"step1_r":{"type":"integer"},"step1_g":{"type":"integer"},"step1_b":{"type":"integer"},"step1_pin":{"type":"integer"},"step1_value":{"type":"integer"},"step1_actuator":{"type":"string"},"step2_action":{"type":"string"},"step2_delay":{"type":"integer"},"step2_message":{"type":"string"},"step2_r":{"type":"integer"},"step2_g":{"type":"integer"},"step2_b":{"type":"integer"},"step2_pin":{"type":"integer"},"step2_value":{"type":"integer"},"step2_actuator":{"type":"string"},"step3_action":{"type":"string"},"step3_delay":{"type":"integer"},"step3_message":{"type":"string"},"step3_r":{"type":"integer"},"step3_g":{"type":"integer"},"step3_b":{"type":"integer"},"step3_pin":{"type":"integer"},"step3_value":{"type":"integer"},"step3_actuator":{"type":"string"},"step4_action":{"type":"string"},"step4_delay":{"type":"integer"},"step4_message":{"type":"string"},"step4_r":{"type":"integer"},"step4_g":{"type":"integer"},"step4_b":{"type":"integer"},"step4_pin":{"type":"integer"},"step4_value":{"type":"integer"},"step4_actuator":{"type":"string"},"step5_action":{"type":"string"},"step5_delay":{"type":"integer"},"step5_message":{"type":"string"},"step5_r":{"type":"integer"},"step5_g":{"type":"integer"},"step5_b":{"type":"integer"},"step5_pin":{"type":"integer"},"step5_value":{"type":"integer"},"step5_actuator":{"type":"string"}},"required":["sensor_name","condition","threshold","step1_action","step2_action"]}}}
])JSON";

// ============================================================
// Individual tool handlers
// ============================================================

static void tool_led_set(const char *args, char *result, int result_len) {
    int r = constrain(toolJsonArgInt(args, "r", 0), 0, 255);
    int g = constrain(toolJsonArgInt(args, "g", 0), 0, 255);
    int b = constrain(toolJsonArgInt(args, "b", 0), 0, 255);
    bool on = (r || g || b);
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
    g_led_user = on;
    snprintf(result, result_len,
             "LED %s (ESP32: no RGB, LED_BUILTIN %s)",
             on ? "ON" : "OFF", on ? "HIGH" : "LOW");
}

static void tool_gpio_write(const char *args, char *result, int result_len) {
    int pin   = toolJsonArgInt(args, "pin", -1);
    int value = toolJsonArgInt(args, "value", 0);
    
    if (pin < 0 || pin > 39 || pin == 1 || pin == 3 || pin == 9 || pin == 10) {
        snprintf(result, result_len,
                 "Error: invalid GPIO %d (valid: 0-39 except 1,3,9,10)", pin);
        return;
    }
    if (pin == FAN_PIN) {
        snprintf(result, result_len,
                 "Error: pin %d is reserved for fan. Use fan_control tool.", pin);
        return;
    }
    if (espDigitalWrite((uint8_t)pin, value)) {
        snprintf(result, result_len, "GPIO %d set to %s", pin, value ? "HIGH" : "LOW");
    } else {
        snprintf(result, result_len, "Error: GPIO write failed on pin %d", pin);
    }
}

static void tool_gpio_read(const char *args, char *result, int result_len) {
    int pin = toolJsonArgInt(args, "pin", -1);
    if (pin < 0 || pin > 39) {
        snprintf(result, result_len, "Error: invalid GPIO %d", pin);
        return;
    }
    int value = espDigitalRead((uint8_t)pin);
    snprintf(result, result_len, "GPIO %d = %d (%s)", pin, value,
             value ? "HIGH" : "LOW");
}

static void tool_fan_control(const char *args, char *result, int result_len) {
    int speed = constrain(toolJsonArgInt(args, "speed", 0), 0, 100);
    int duty  = map(speed, 0, 100, 0, 255);
    if (espPWMWrite(FAN_PIN, duty)) {
        snprintf(result, result_len, "Fan set to %d%% (duty=%d/255)", speed, duty);
    } else {
        snprintf(result, result_len, "Error: Fan PWM failed");
    }
}

static void tool_device_info(const char *args, char *result, int result_len) {
    (void)args;
    snprintf(result, result_len,
        "Free heap: %u bytes, Total heap: %u bytes, "
        "Uptime: %lu seconds, "
        "WiFi: %s, IP: %s, "
        "Chip: %s rev %d, %d cores, %lu MHz",
        ESP.getFreeHeap(), ESP.getHeapSize(),
        millis() / 1000,
        WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
        WiFi.localIP().toString().c_str(),
        ESP.getChipModel(), ESP.getChipRevision(),
        ESP.getChipCores(), ESP.getCpuFreqMHz());
}

static void tool_file_read(const char *args, char *result, int result_len) {
    char path[64];
    if (!toolJsonArgString(args, "path", path, sizeof(path))) {
        snprintf(result, result_len, "Error: missing 'path'");
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        snprintf(result, result_len, "Error: file not found: %s", path);
        return;
    }
    int len = f.readBytes(result, result_len - 1);
    result[len] = '\0';
    f.close();
}

static void tool_file_write(const char *args, char *result, int result_len) {
    char path[64] = "", content[480] = "";
    if (!toolJsonArgString(args, "path", path, sizeof(path))) {
        snprintf(result, result_len, "Error: missing path"); return;
    }
    if (path[0] != '/') {
        char fixed[66] = "/";
        strncat(fixed, path, sizeof(fixed)-2);
        strncpy(path, fixed, sizeof(path)-1);
    }
    
    if (!toolJsonArgString(args, "content", content, sizeof(content))) {
        snprintf(result, result_len, "Error: missing 'content'");
        return;
    }
    File f = LittleFS.open(path, "w");
    if (!f) {
        snprintf(result, result_len, "Error: cannot open %s for writing", path);
        return;
    }
    f.print(content);
    f.close();
    snprintf(result, result_len, "Written %d bytes to %s", (int)strlen(content), path);
}

static void tool_temperature_read(const char *args, char *result, int result_len) {
    (void)args;
    float temp = espInternalTemp();
    if (temp == 0.0f) {
        snprintf(result, result_len,
                 "Chip temp: N/A (classic ESP32 has no internal temp sensor). "
                 "Register an external NTC sensor via device_register.");
    } else {
        snprintf(result, result_len, "Chip temp: %.1f C", temp);
    }
}

static void tool_device_register(const char *args, char *result, int result_len) {
    char name[DEV_NAME_LEN];
    char type_str[24];
    if (!toolJsonArgString(args, "name", name, sizeof(name))) {
        snprintf(result, result_len, "Error: missing 'name'"); return;
    }
    if (!toolJsonArgString(args, "type", type_str, sizeof(type_str))) {
        snprintf(result, result_len, "Error: missing 'type'"); return;
    }

    int pin         = toolJsonArgInt(args, "pin", PIN_NONE);
    bool inverted   = toolJsonArgBool(args, "inverted", false);
    char unit[DEV_UNIT_LEN] = "";
    toolJsonArgString(args, "unit", unit, sizeof(unit));

    DeviceKind kind;
    if (strcmp(type_str, "digital_in") == 0)  kind = DEV_SENSOR_DIGITAL;
    else if (strcmp(type_str, "analog_in") == 0) kind = DEV_SENSOR_ANALOG_RAW;
    else if (strcmp(type_str, "ntc_10k") == 0)   kind = DEV_SENSOR_NTC_10K;
    else if (strcmp(type_str, "ldr") == 0)        kind = DEV_SENSOR_LDR;
    else if (strcmp(type_str, "digital_out") == 0) kind = DEV_ACTUATOR_DIGITAL;
    else if (strcmp(type_str, "relay") == 0)       kind = DEV_ACTUATOR_RELAY;
    else if (strcmp(type_str, "pwm") == 0)         kind = DEV_ACTUATOR_PWM;
    else {
        snprintf(result, result_len, "Error: unknown type '%s'", type_str);
        return;
    }

    if (!deviceRegister(name, kind, (uint8_t)pin, unit[0] ? unit : NULL,
                        inverted, NULL, 0)) {
        snprintf(result, result_len,
                 "Error: device '%s' already exists or registry full", name);
        return;
    }

    devicesSave();
    snprintf(result, result_len, "Registered %s '%s' on GPIO %d",
             type_str, name, pin);
}

static void tool_device_list(const char *args, char *result, int result_len) {
    (void)args;
    int w = 0, count = 0;
    Device *devs = deviceGetAll();

    for (int i = 0; i < MAX_DEVICES && w < result_len - 80; i++) {
        if (!devs[i].used) continue;
        Device *d = &devs[i];
        if (count > 0) w += snprintf(result + w, result_len - w, ", ");

        if (deviceIsSensor(d->kind)) {
            float val = deviceReadSensor(d, false);
            w += snprintf(result + w, result_len - w,
                          "%s(%s pin%d)=%.1f%s",
                          d->name, deviceKindName(d->kind), d->pin, val, d->unit);
        } else {
            w += snprintf(result + w, result_len - w,
                          "%s(%s pin%d%s)",
                          d->name, deviceKindName(d->kind), d->pin,
                          d->inverted ? " inv" : "");
        }
        count++;
    }
    if (count == 0) snprintf(result, result_len, "No devices registered");
}

static void tool_device_remove(const char *args, char *result, int result_len) {
    char name[DEV_NAME_LEN];
    if (!toolJsonArgString(args, "name", name, sizeof(name))) {
        snprintf(result, result_len, "Error: missing 'name'"); return;
    }
    if (!deviceRemove(name)) {
        snprintf(result, result_len, "Error: device '%s' not found", name); return;
    }
    devicesSave();
    snprintf(result, result_len, "Removed device '%s'", name);
}

static void tool_sensor_read(const char *args, char *result, int result_len) {
    char name[DEV_NAME_LEN];
    if (!toolJsonArgString(args, "name", name, sizeof(name))) {
        snprintf(result, result_len, "Error: missing 'name'"); return;
    }
    Device *dev = deviceFind(name);
    if (!dev) { snprintf(result, result_len, "Error: sensor '%s' not found", name); return; }
    if (!deviceIsSensor(dev->kind)) {
        snprintf(result, result_len, "Error: '%s' is not a sensor", name); return;
    }
    float val = deviceReadSensor(dev, false);
    snprintf(result, result_len, "%s: %.1f %s", name, val, dev->unit);
}

static void tool_actuator_set(const char *args, char *result, int result_len) {
    char name[DEV_NAME_LEN];
    if (!toolJsonArgString(args, "name", name, sizeof(name))) {
        snprintf(result, result_len, "Error: missing 'name'"); return;
    }
    int value = toolJsonArgInt(args, "value", 0);
    Device *dev = deviceFind(name);
    if (!dev) { snprintf(result, result_len, "Error: actuator '%s' not found", name); return; }
    if (!deviceIsActuator(dev->kind)) {
        snprintf(result, result_len, "Error: '%s' is not an actuator", name); return;
    }
    if (!deviceSetActuator(dev, value)) {
        snprintf(result, result_len, "Error: failed to set '%s'", name); return;
    }
    snprintf(result, result_len, "Set %s to %d", name, value);
}

// ============================================================
// Rule tools
// ============================================================

static ActionType parseActionType(const char *s) {
    if (strcmp(s, "gpio_write") == 0)  return ACT_GPIO_WRITE;
    if (strcmp(s, "led_set") == 0)     return ACT_LED_SET;
    if (strcmp(s, "actuator") == 0)    return ACT_ACTUATOR;
    if (strcmp(s, "serial_send") == 0) return ACT_SERIAL_SEND;
    return ACT_GPIO_WRITE;
}

static ConditionOp parseConditionOp(const char *s) {
    if (strcmp(s, "gt") == 0)      return COND_GT;
    if (strcmp(s, "lt") == 0)      return COND_LT;
    if (strcmp(s, "eq") == 0)      return COND_EQ;
    if (strcmp(s, "neq") == 0)     return COND_NEQ;
    if (strcmp(s, "change") == 0)  return COND_CHANGE;
    if (strcmp(s, "always") == 0)  return COND_ALWAYS;
    if (strcmp(s, "chained") == 0) return COND_CHAINED;
    return COND_GT;
}

static void tool_rule_create(const char *args, char *result, int result_len) {
    char rule_name[RULE_NAME_LEN];
    if (!toolJsonArgString(args, "rule_name", rule_name, sizeof(rule_name))) {
        snprintf(result, result_len, "Error: missing 'rule_name'"); return;
    }

    char sensor_name[DEV_NAME_LEN] = "";
    toolJsonArgString(args, "sensor_name", sensor_name, sizeof(sensor_name));
    uint8_t sensor_pin = (uint8_t)toolJsonArgInt(args, "sensor_pin", PIN_NONE);

    char cond_str[16];
    if (!toolJsonArgString(args, "condition", cond_str, sizeof(cond_str))) {
        if (!sensor_name[0] && sensor_pin == PIN_NONE) strcpy(cond_str, "chained");
        else { snprintf(result, result_len, "Error: missing 'condition'"); return; }
    }
    ConditionOp condition = parseConditionOp(cond_str);
    int32_t threshold = toolJsonArgInt(args, "threshold", 0);

    if (condition != COND_CHAINED) {
        if (sensor_name[0]) {
            Device *dev = deviceFind(sensor_name);
            if (!dev) {
                snprintf(result, result_len,
                         "Error: sensor '%s' not found", sensor_name);
                return;
            }
        } else if (sensor_pin == PIN_NONE) {
            snprintf(result, result_len, "Error: provide sensor_name or sensor_pin");
            return;
        }
    }

    int interval_s = toolJsonArgInt(args, "interval_seconds", 5);
    if (interval_s < 5) interval_s = 5;
    uint32_t interval_ms = (uint32_t)interval_s * 1000;

    ActionType on_action = ACT_GPIO_WRITE;
    char on_actuator[DEV_NAME_LEN] = "";
    uint8_t on_pin   = 0;
    int32_t on_value = 1;
    char on_nats_subj[RULE_NATS_SUBJ_LEN] = "";
    char on_nats_pay[RULE_NATS_PAY_LEN]   = "";

    bool has_off = false;
    ActionType off_action = ACT_GPIO_WRITE;
    char off_actuator[DEV_NAME_LEN] = "";
    uint8_t off_pin   = 0;
    int32_t off_value = 0;
    char off_nats_subj[RULE_NATS_SUBJ_LEN] = "";
    char off_nats_pay[RULE_NATS_PAY_LEN]   = "";

    char actuator_name[DEV_NAME_LEN] = "";
    toolJsonArgString(args, "actuator_name", actuator_name, sizeof(actuator_name));

    if (actuator_name[0]) {
        Device *act = deviceFind(actuator_name);
        if (!act || !deviceIsActuator(act->kind)) {
            snprintf(result, result_len, "Error: actuator '%s' not found", actuator_name);
            return;
        }
        on_action = ACT_ACTUATOR;
        strncpy(on_actuator, actuator_name, DEV_NAME_LEN - 1);
        has_off = true;
        off_action = ACT_ACTUATOR;
        strncpy(off_actuator, actuator_name, DEV_NAME_LEN - 1);
    } else {
        char act_str[24] = "";
        toolJsonArgString(args, "on_action", act_str, sizeof(act_str));
        if (act_str[0]) on_action = parseActionType(act_str);

        on_pin   = (uint8_t)toolJsonArgInt(args, "on_pin", 0);
        on_value = toolJsonArgInt(args, "on_value", 1);
        toolJsonArgString(args, "on_serial_text", on_nats_pay, sizeof(on_nats_pay));

        if (on_action == ACT_LED_SET && toolJsonArgExists(args, "on_r")) {
            int r = constrain(toolJsonArgInt(args, "on_r", 0), 0, 255);
            int g = constrain(toolJsonArgInt(args, "on_g", 0), 0, 255);
            int b = constrain(toolJsonArgInt(args, "on_b", 0), 0, 255);
            on_value = (r << 16) | (g << 8) | b;
        }
    }

    char off_act_str[24] = "";
    toolJsonArgString(args, "off_action", off_act_str, sizeof(off_act_str));
    if (off_act_str[0]) {
        if (strcmp(off_act_str, "none") == 0) {
            has_off = false;
        } else if (strcmp(off_act_str, "auto") == 0) {
            has_off = true;
            off_action = on_action;
            strncpy(off_actuator, on_actuator, DEV_NAME_LEN - 1);
            off_pin   = on_pin;
            off_value = 0;
            if (off_action == ACT_LED_SET && toolJsonArgExists(args, "off_r")) {
                int r = constrain(toolJsonArgInt(args, "off_r", 0), 0, 255);
                int g = constrain(toolJsonArgInt(args, "off_g", 0), 0, 255);
                int b = constrain(toolJsonArgInt(args, "off_b", 0), 0, 255);
                off_value = (r << 16) | (g << 8) | b;
            }
            if (off_action == ACT_SERIAL_SEND)
                toolJsonArgString(args, "off_serial_text", off_nats_pay, sizeof(off_nats_pay));
        } else {
            has_off    = true;
            off_action = parseActionType(off_act_str);
            off_pin    = (uint8_t)toolJsonArgInt(args, "off_pin", 0);
            off_value  = toolJsonArgInt(args, "off_value", 0);
            if (off_action == ACT_LED_SET && toolJsonArgExists(args, "off_r")) {
                int r = constrain(toolJsonArgInt(args, "off_r", 0), 0, 255);
                int g = constrain(toolJsonArgInt(args, "off_g", 0), 0, 255);
                int b = constrain(toolJsonArgInt(args, "off_b", 0), 0, 255);
                off_value = (r << 16) | (g << 8) | b;
            }
            if (off_action == ACT_SERIAL_SEND)
                toolJsonArgString(args, "off_serial_text", off_nats_pay, sizeof(off_nats_pay));
        }
    }

    char chain_rule[RULE_ID_LEN]     = "";
    char chain_off_rule[RULE_ID_LEN] = "";
    toolJsonArgString(args, "chain_rule",     chain_rule,     sizeof(chain_rule));
    toolJsonArgString(args, "chain_off_rule", chain_off_rule, sizeof(chain_off_rule));
    uint32_t chain_delay_ms     = (uint32_t)toolJsonArgInt(args, "chain_delay_seconds", 0) * 1000;
    uint32_t chain_off_delay_ms = (uint32_t)toolJsonArgInt(args, "chain_off_delay_seconds", 0) * 1000;

    const char *id = ruleCreate(rule_name, sensor_name, sensor_pin, false,
                                condition, threshold, interval_ms,
                                on_action, on_actuator, on_pin, on_value,
                                on_nats_subj, on_nats_pay,
                                has_off, off_action, off_actuator,
                                off_pin, off_value, off_nats_subj, off_nats_pay,
                                chain_rule, chain_delay_ms,
                                chain_off_rule, chain_off_delay_ms);

    if (!id) {
        snprintf(result, result_len, "Error: rule creation failed (max %d rules)", MAX_RULES);
        return;
    }

    rulesSave();

    const char *cond_sym = "?";
    switch (condition) {
        case COND_GT: cond_sym = ">"; break;
        case COND_LT: cond_sym = "<"; break;
        case COND_EQ: cond_sym = "=="; break;
        case COND_NEQ: cond_sym = "!="; break;
        case COND_CHANGE: cond_sym = "changed"; break;
        case COND_ALWAYS: cond_sym = "always"; break;
        case COND_CHAINED: cond_sym = "chained"; break;
    }

    if (condition == COND_CHAINED) {
        snprintf(result, result_len, "Rule created: %s '%s' - chained", id, rule_name);
    } else {
        const char *src = sensor_name[0] ? sensor_name : "pin";
        snprintf(result, result_len, "Rule created: %s '%s' - %s %s %d (every %ds)%s",
                 id, rule_name, src, cond_sym, (int)threshold, interval_s,
                 has_off ? " with auto-off" : "");
    }
}

static void tool_rule_list(const char *args, char *result, int result_len) {
    (void)args;
    const Rule *rules = ruleGetAll();
    int w = 0, count = 0;

    for (int i = 0; i < MAX_RULES && w < result_len - 60; i++) {
        if (!rules[i].used) continue;
        const Rule *r = &rules[i];
        if (count > 0) w += snprintf(result + w, result_len - w, "; ");

        uint32_t ago = r->last_triggered ? (millis() - r->last_triggered) / 1000 : 0;
        w += snprintf(result + w, result_len - w,
            "%s '%s' %s %s %d val=%.1f %s",
            r->id, r->name,
            r->enabled ? "ON" : "OFF",
            r->sensor_name[0] ? r->sensor_name : "pin",
            (int)r->threshold, r->last_reading,
            r->fired ? "FIRED" : "idle");
        if (r->chain_id[0])
            w += snprintf(result + w, result_len - w, " ->%s", r->chain_id);
        count++;
    }
    if (count == 0) snprintf(result, result_len, "No rules defined");
}

static void tool_rule_delete(const char *args, char *result, int result_len) {
    char rule_id[RULE_ID_LEN];
    if (!toolJsonArgString(args, "rule_id", rule_id, sizeof(rule_id))) {
        snprintf(result, result_len, "Error: missing 'rule_id'"); return;
    }
    if (!ruleDelete(rule_id)) {
        snprintf(result, result_len, "Error: rule '%s' not found", rule_id); return;
    }
    rulesSave();
    if (strcmp(rule_id, "all") == 0)
        snprintf(result, result_len, "All rules deleted");
    else
        snprintf(result, result_len, "Deleted rule %s", rule_id);
}

static void tool_rule_enable(const char *args, char *result, int result_len) {
    char rule_id[RULE_ID_LEN];
    if (!toolJsonArgString(args, "rule_id", rule_id, sizeof(rule_id))) {
        snprintf(result, result_len, "Error: missing 'rule_id'"); return;
    }
    bool enabled = toolJsonArgBool(args, "enabled", true);
    if (!ruleEnable(rule_id, enabled)) {
        snprintf(result, result_len, "Error: rule '%s' not found", rule_id); return;
    }
    rulesSave();
    snprintf(result, result_len, "Rule %s %s", rule_id, enabled ? "enabled" : "disabled");
}

static void tool_serial_send(const char *args, char *result, int result_len) {
    char text[120];
    if (!toolJsonArgString(args, "text", text, sizeof(text))) {
        snprintf(result, result_len, "Error: missing 'text'"); return;
    }
    Serial.printf("[SERIAL_SEND] %s\n", text);
    snprintf(result, result_len, "Sent to Serial: %s", text);
}

static void tool_chain_create(const char *args, char *result, int result_len) {
    char sensor_name[DEV_NAME_LEN] = "";
    if (!toolJsonArgString(args, "sensor_name", sensor_name, sizeof(sensor_name))) {
        snprintf(result, result_len, "Error: missing 'sensor_name'"); return;
    }
    char cond_str[16] = "";
    if (!toolJsonArgString(args, "condition", cond_str, sizeof(cond_str))) {
        snprintf(result, result_len, "Error: missing 'condition'"); return;
    }
    ConditionOp condition = parseConditionOp(cond_str);
    int32_t threshold = toolJsonArgInt(args, "threshold", 0);
    int interval_s = toolJsonArgInt(args, "interval_seconds", 5);
    if (interval_s < 5) interval_s = 5;
    uint32_t interval_ms = (uint32_t)interval_s * 1000;

    Device *dev = deviceFind(sensor_name);
    if (!dev) {
        snprintf(result, result_len, "Error: sensor '%s' not found", sensor_name); return;
    }

    struct StepInfo {
        ActionType action;
        uint8_t    pin;
        int32_t    value;
        char       nats_subj[RULE_NATS_SUBJ_LEN];
        char       nats_pay[RULE_NATS_PAY_LEN];
        char       actuator[DEV_NAME_LEN];
        uint32_t   delay_ms;
        bool       used;
    };

    static const char *prefixes[5] = {"step1","step2","step3","step4","step5"};
    StepInfo steps[5];
    memset(steps, 0, sizeof(steps));

    for (int s = 0; s < 5; s++) {
        char key[32], act_str[24] = "";
        snprintf(key, sizeof(key), "%s_action", prefixes[s]);
        toolJsonArgString(args, key, act_str, sizeof(act_str));

        if (!act_str[0]) {
            if (s < 2) {
                snprintf(result, result_len, "Error: missing '%s'", key); return;
            }
            break;
        }
        steps[s].used   = true;
        steps[s].action = parseActionType(act_str);

        if (s > 0) {
            snprintf(key, sizeof(key), "%s_delay", prefixes[s]);
            steps[s].delay_ms = (uint32_t)toolJsonArgInt(args, key, 0) * 1000;
        }

        snprintf(key, sizeof(key), "%s_message", prefixes[s]);
        toolJsonArgString(args, key, steps[s].nats_pay, RULE_NATS_PAY_LEN);
        snprintf(key, sizeof(key), "%s_actuator", prefixes[s]);
        toolJsonArgString(args, key, steps[s].actuator, DEV_NAME_LEN);
        snprintf(key, sizeof(key), "%s_pin", prefixes[s]);
        steps[s].pin = (uint8_t)toolJsonArgInt(args, key, 0);
        snprintf(key, sizeof(key), "%s_value", prefixes[s]);
        steps[s].value = toolJsonArgInt(args, key, 1);

        if (steps[s].action == ACT_LED_SET) {
            snprintf(key, sizeof(key), "%s_r", prefixes[s]);
            if (toolJsonArgExists(args, key)) {
                int r = constrain(toolJsonArgInt(args, key, 0), 0, 255);
                snprintf(key, sizeof(key), "%s_g", prefixes[s]);
                int g = constrain(toolJsonArgInt(args, key, 0), 0, 255);
                snprintf(key, sizeof(key), "%s_b", prefixes[s]);
                int b = constrain(toolJsonArgInt(args, key, 0), 0, 255);
                steps[s].value = (r << 16) | (g << 8) | b;
            }
        }
    }

    int num_steps = 0;
    for (int s = 0; s < 5; s++) { if (steps[s].used) num_steps = s + 1; }

    const char *ids[5] = {NULL};

    for (int i = num_steps - 1; i >= 0; i--) {
        StepInfo *st = &steps[i];
        const char *chain_id    = (i < num_steps - 1) ? ids[i + 1] : NULL;
        uint32_t chain_delay    = (i < num_steps - 1) ? steps[i + 1].delay_ms : 0;
        bool is_source          = (i == 0);

        char name[RULE_NAME_LEN];
        snprintf(name, sizeof(name), "%s step%d", sensor_name, i + 1);

        const char *id = ruleCreate(
            name,
            is_source ? sensor_name : "",
            PIN_NONE, false,
            is_source ? condition : COND_CHAINED,
            is_source ? threshold : 0,
            interval_ms,
            st->action, st->actuator, st->pin, st->value,
            st->nats_subj, st->nats_pay,
            false, ACT_GPIO_WRITE, "", 0, 0, "", "",
            chain_id, chain_delay, NULL, 0
        );

        if (!id) {
            snprintf(result, result_len, "Error: max rules reached at step %d", i + 1);
            return;
        }
        ids[i] = id;
    }

    rulesSave();
    int w = snprintf(result, result_len, "Chain created: %s %s>%d",
                     ids[0], sensor_name, (int)threshold);
    for (int i = 0; i < num_steps && w < result_len - 30; i++) {
        w += snprintf(result + w, result_len - w, " -> step%d(%s)", i + 1,
                      actionTypeName(steps[i].action));
    }
}

// ============================================================
// Public API
// ============================================================

const char *toolsGetDefinitions() { return TOOLS_JSON; }

bool toolExecute(const char *name, const char *args_json,
                 char *result, int result_len) {
    if      (strcmp(name, "led_set") == 0)        tool_led_set(args_json, result, result_len);
    else if (strcmp(name, "gpio_write") == 0)      tool_gpio_write(args_json, result, result_len);
    else if (strcmp(name, "gpio_read") == 0)       tool_gpio_read(args_json, result, result_len);
    else if (strcmp(name, "fan_control") == 0)     tool_fan_control(args_json, result, result_len);
    else if (strcmp(name, "device_info") == 0)     tool_device_info(args_json, result, result_len);
    else if (strcmp(name, "file_read") == 0)       tool_file_read(args_json, result, result_len);
    else if (strcmp(name, "file_write") == 0)      tool_file_write(args_json, result, result_len);
    else if (strcmp(name, "temperature_read") == 0) tool_temperature_read(args_json, result, result_len);
    else if (strcmp(name, "device_register") == 0) tool_device_register(args_json, result, result_len);
    else if (strcmp(name, "device_list") == 0)     tool_device_list(args_json, result, result_len);
    else if (strcmp(name, "device_remove") == 0)   tool_device_remove(args_json, result, result_len);
    else if (strcmp(name, "sensor_read") == 0)     tool_sensor_read(args_json, result, result_len);
    else if (strcmp(name, "actuator_set") == 0)    tool_actuator_set(args_json, result, result_len);
    else if (strcmp(name, "rule_create") == 0)     tool_rule_create(args_json, result, result_len);
    else if (strcmp(name, "rule_list") == 0)       tool_rule_list(args_json, result, result_len);
    else if (strcmp(name, "rule_delete") == 0)     tool_rule_delete(args_json, result, result_len);
    else if (strcmp(name, "rule_enable") == 0)     tool_rule_enable(args_json, result, result_len);
    else if (strcmp(name, "serial_send") == 0)     tool_serial_send(args_json, result, result_len);
    else if (strcmp(name, "chain_create") == 0)    tool_chain_create(args_json, result, result_len);
    else {
        snprintf(result, result_len, "Error: unknown tool '%s'", name);
        return false;
    }
    return true;
}