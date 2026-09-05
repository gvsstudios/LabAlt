/**
 * config.h  -  WireClaw ESP32 Classic (STANDALONE - NO UNO)
 * 
 * All sensors/actuators are directly connected to ESP32 GPIO pins.
 * ADC is 12-bit (0-4095) for analog reads.
 * PWM uses LEDC channels (0-15).
 */

#ifndef CONFIG_H
#define CONFIG_H
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <stdlib.h>
#include <string.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

#define WIRECLAW_VERSION "0.5.0-esp32-standalone"

/* ============================================================
   Runtime config
   ============================================================ */
extern char cfg_wifi_ssid[64];
extern char cfg_wifi_pass[64];
extern char cfg_api_key[128];
extern char cfg_model[64];
extern char cfg_device_name[32];
extern char cfg_api_base_url[128];
extern char cfg_system_prompt[2048];
extern char cfg_timezone[64];

extern bool g_debug;
extern bool g_led_user;

void led_on(void);
void led_off_hw(void);
void configSave(void);

/* ============================================================
   LLM sizes
   ============================================================ */
#define LLM_MAX_RESPONSE_LEN   1024
#define LLM_MAX_REQUEST_LEN    4096
#define LLM_READ_TIMEOUT_MS    120000
#define LLM_MAX_MESSAGES       7
#define LLM_MAX_TOOL_CALLS     3

typedef struct {
    char id[64];
    char name[32];
    char arguments[246];
} LlmToolCall;

#define LLM_MSG_NORMAL      0
#define LLM_MSG_TOOL_CALL   1
#define LLM_MSG_TOOL_RESULT 2

typedef struct {
    int         type;
    const char *role;
    const char *content;
    const char *tool_call_id;
    const char *tool_calls_json;
} LlmMessage;

typedef struct {
    bool ok;
    char content[LLM_MAX_RESPONSE_LEN];
    int  content_len;
    int  http_status;
    int  prompt_tokens;
    int  completion_tokens;
    LlmToolCall tool_calls[LLM_MAX_TOOL_CALLS];
    int  tool_call_count;
    char tool_calls_json[2048];
} LlmResult;

LlmMessage  llmMsg(const char *role, const char *content);
LlmMessage  llmToolResult(const char *tool_call_id, const char *content);
LlmMessage  llmToolCallMsg(const char *content, const char *tool_calls_json);

void        llmBegin(const char *api_key, const char *model, const char *base_url);
bool        llmChat(const LlmMessage *messages, int count,
                    const char *tools_json, LlmResult *result);
const char *llmLastError(void);

/* ============================================================
   Device registry - ALL PINS ON ESP32 NOW
   ============================================================ */
#define MAX_DEVICES   16
#define DEV_NAME_LEN  24
#define DEV_UNIT_LEN  8
#define PIN_NONE      255
#define DEV_HISTORY_LEN 6

typedef enum {
    DEV_SENSOR_DIGITAL = 0,
    DEV_SENSOR_ANALOG_RAW,
    DEV_SENSOR_NTC_10K,
    DEV_SENSOR_LDR,
    DEV_SENSOR_INTERNAL_TEMP,
    DEV_SENSOR_CLOCK_HOUR,
    DEV_SENSOR_CLOCK_MINUTE,
    DEV_SENSOR_CLOCK_HHMM,
    DEV_SENSOR_NATS_VALUE,
    DEV_ACTUATOR_DIGITAL,
    DEV_ACTUATOR_RELAY,
    DEV_ACTUATOR_PWM,
    DEV_ACTUATOR_RGB_LED
} DeviceKind;

typedef struct {
    char       name[DEV_NAME_LEN];
    DeviceKind kind;
    uint8_t    pin;
    char       unit[DEV_UNIT_LEN];
    bool       inverted;
    bool       used;
    char       nats_subject[32];
    float      nats_value;
    char       nats_msg[64];
    uint16_t   nats_sid;
    uint32_t   baud;
    int        last_value;
    float      ema;
    bool       ema_init;
    float      history[DEV_HISTORY_LEN];
    uint8_t    history_idx;
    bool       history_full;
} Device;

/* ---- Device API ---- */
bool        deviceRegister(const char *name, DeviceKind kind, uint8_t pin,
                            const char *unit, bool inverted,
                            const char *nats_subject, uint32_t baud);
#define deviceRegisterSimple(n,k,p,u,i) \
    deviceRegister((n),(k),(p),(u),(i),NULL,0)

bool        deviceRemove(const char *name);
Device     *deviceFind(const char *name);
Device     *deviceGetAll(void);
Device     *deviceGetAllMutable(void);

bool        deviceIsSensor(DeviceKind kind);
bool        deviceIsActuator(DeviceKind kind);
const char *deviceKindName(DeviceKind kind);

float       deviceReadSensor(Device *dev, bool record_hist);
#define deviceReadSensorNow(dev) deviceReadSensor((dev), false)

bool        deviceSetActuator(Device *dev, int value);
void        deviceSetNatsValue(Device *dev, float value, const char *msg);
const char *deviceGetNatsMsg(const Device *dev);

void        devicesInit(void);
void        devicesSave(void);
void        devicesClear(void);
void        devicesReload(void);
void        sensorsPoll(void);

/* ============================================================
   ESP32 PIN DIRECT ACCESS - NO UNO
   ============================================================ */
// Digital read/write helpers
int  espDigitalRead(uint8_t pin);
bool espDigitalWrite(uint8_t pin, int value);

// Analog read (ESP32 ADC: 12-bit, 0-4095, ~0-3.3V)
int  espAnalogRead(uint8_t pin);

// PWM via LEDC (0-255 duty, channel auto-assigned)
bool espPWMWrite(uint8_t pin, int duty);

// Internal temperature sensor (ESP32 built-in, if available)
float espInternalTemp(void);

/* ============================================================
   FAN - Direct ESP32 PWM pin
   ============================================================ */
#define FAN_PIN     25   // GPIO25 supports PWM via LEDC

/* ============================================================
   Rule engine (unchanged except actuator calls)
   ============================================================ */
#define MAX_RULES          16
#define RULE_NAME_LEN      32
#define RULE_NATS_SUBJ_LEN 64
#define RULE_NATS_PAY_LEN  64
#define RULE_ID_LEN        12
#define MAX_PENDING_CHAINS 8
#define MAX_CHAIN_DEPTH    8

typedef enum {
    COND_GT, COND_LT, COND_EQ, COND_NEQ,
    COND_CHANGE, COND_ALWAYS, COND_CHAINED
} ConditionOp;

typedef enum {
    ACT_GPIO_WRITE,
    ACT_LED_SET,
    ACT_NATS_PUBLISH,
    ACT_ACTUATOR,
    ACT_TELEGRAM,
    ACT_SERIAL_SEND
} ActionType;

typedef struct {
    char id[RULE_ID_LEN];
    char name[RULE_NAME_LEN];
    char sensor_name[DEV_NAME_LEN];
    uint8_t sensor_pin;
    bool sensor_analog;
    ConditionOp condition;
    int32_t threshold;
    ActionType on_action;
    char on_actuator[DEV_NAME_LEN];
    uint8_t on_pin;
    int32_t on_value;
    char on_nats_subj[RULE_NATS_SUBJ_LEN];
    char on_nats_pay[RULE_NATS_PAY_LEN];
    bool has_off_action;
    ActionType off_action;
    char off_actuator[DEV_NAME_LEN];
    uint8_t off_pin;
    int32_t off_value;
    char off_nats_subj[RULE_NATS_SUBJ_LEN];
    char off_nats_pay[RULE_NATS_PAY_LEN];
    uint32_t interval_ms;
    uint32_t last_eval;
    uint32_t last_triggered;
    uint32_t last_telegram_ms;
    char chain_id[RULE_ID_LEN];
    uint32_t chain_delay_ms;
    char chain_off_id[RULE_ID_LEN];
    uint32_t chain_off_delay_ms;
    bool fired;
    float last_reading;
    uint32_t last_msg_hash;
    bool enabled;
    bool used;
} Rule;

const Rule *ruleGetAll(void);
Rule       *ruleFind(const char *id);
const char *ruleCreate(...); // same signature as before
bool        ruleDelete(const char *id);
bool        ruleEnable(const char *id, bool enable);
void        rulesSave(void);
void        rulesInit(void);
void        rulesEvaluate(void);
const char *conditionOpName(ConditionOp op);
const char *actionTypeName(ActionType act);

/* ============================================================
   Tools API
   ============================================================ */
const char *toolsGetDefinitions(void);
bool        toolExecute(const char *name, const char *args_json,
                         char *result, int result_len);

/* ============================================================
   Web config API
   ============================================================ */
void webConfigInit(void);
void webConfigHandle(void);

#define TOOL_RESULT_MAX_LEN 384

#endif /* CONFIG_H */