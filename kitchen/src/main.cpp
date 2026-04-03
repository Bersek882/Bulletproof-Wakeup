// ═══════════════════════════════════════════════════════════════════════════
// BULLETPROOF WAKEUP — Kitchen (Master)
// Device: M5StickC Plus 2
// Role: NTP time source, web UI, alarm trigger, sole stop switch.
// ═══════════════════════════════════════════════════════════════════════════

#include <M5Unified.h>
#include <esp_now.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "webui.h"

// mDNS hostname: device reachable at http://alarm.local
#define MDNS_HOSTNAME "alarm"

// ── Timezone & NTP ────────────────────────────────────────────────────────────
#define NTP_SERVER          "pool.ntp.org"

// ── Intervals ─────────────────────────────────────────────────────────────────
#define TIME_SYNC_MS        60000UL   // Time sync to bedroom: every minute
#define ALARM_RESEND_MS      5000UL   // Repeat ALARM_START until ACK received
#define NTP_RESYNC_CYCLES      60     // Re-sync NTP every 60 min (= 60 × 60s)
#define DISPLAY_UPDATE_MS    1000UL

// ── Confirmation timeouts ─────────────────────────────────────────────────────
#define CONFIRM_TIMEOUT_MS  30000UL   // 30s window to confirm via BtnA
#define CONFIRM_DONE_TTL_MS 10000UL   // Keep "confirmed" state for 10s for web UI
#define CONFIRM_THRESHOLD_H 5         // Confirmation only required if alarm is <= 5h away

// ── Snooze: repeated alarm after stop ────────────────────────────────────────
#define SNOOZE_DELAY_SHORT_MS 60000UL  // 1 minute pause (snooze 1+2)
#define SNOOZE_DELAY_LONG_MS 120000UL  // 2 minute pause (snooze 3)
#define SNOOZE_MAX_CYCLES    3         // Max 3 snooze cycles, then silence
#define KITCHEN_TONE_HZ      4000      // Kitchen buzzer frequency (piezo resonance)
#define KITCHEN_PULSE_ON_MS  600       // Pulse: 600ms on
#define KITCHEN_PULSE_OFF_MS 100       // Pulse: 100ms off

// ── Display brightness (M5StickC Plus 2, 0–255) ──────────────────────────────
#define BRIGHTNESS_NORMAL   80
#define BRIGHTNESS_ALARM    220

// ── Watchdog ─────────────────────────────────────────────────────────────────
#define WDT_TIMEOUT_SEC     15

// ═══════════════════════════════════════════════════════════════════════════
// Message protocol (identical to bedroom/src/main.cpp)
// ═══════════════════════════════════════════════════════════════════════════
enum MsgType : uint8_t {
    MSG_TIME_SYNC     = 0x01,
    MSG_ALARM_SET     = 0x02,
    MSG_ALARM_START   = 0x03,
    MSG_ALARM_STOP    = 0x04,
    MSG_ACK           = 0x05,
    MSG_ALARM_DISABLE = 0x06,  // Fully disable alarm
};

struct __attribute__((packed)) AlarmMessage {
    MsgType  type;
    uint32_t epoch;
    uint8_t  alarmH;
    uint8_t  alarmM;
    uint8_t  msgId;
};

// ── Confirmation state ────────────────────────────────────────────────────────
enum ConfirmAction : uint8_t {
    CONFIRM_NONE    = 0,
    CONFIRM_SET     = 1,   // Set new alarm time
    CONFIRM_DISABLE = 2,   // Disable alarm
};

// ── Global state variables ────────────────────────────────────────────────────
uint8_t peerMac[]    = PEER_MAC;
uint8_t alarmHour    = 7;
uint8_t alarmMinute  = 0;
bool    alarmEnabled = true;

bool alarmActive      = false;
bool alarmAckReceived = false;
uint8_t msgCounter    = 0;

// Confirmation: wait for BtnA press before applying setting
ConfirmAction confirmPending   = CONFIRM_NONE;
uint8_t       confirmHour      = 0;
uint8_t       confirmMinute    = 0;
unsigned long confirmRequestMs = 0;
bool          confirmDone      = false;  // true: BtnA pressed, action executed
unsigned long confirmDoneMs    = 0;      // timestamp of confirmation (for TTL)

// ── Snooze state ─────────────────────────────────────────────────────────────
bool          snoozeActive    = false;  // snooze countdown running
unsigned long snoozeStartMs   = 0;      // timestamp of last alarm stop
uint8_t       snoozeCount     = 0;      // how many snooze cycles have fired so far
bool          kitchenBuzzing  = false;  // kitchen buzzer active (only during snooze alarm)
bool          kitchenPulseOn  = false;
unsigned long kitchenPulseMs  = 0;

// ── Web lock: blocks web UI during snooze phase ──────────────────────────────
bool          webLocked       = false;  // true = web settings locked until all snooze cycles are done

volatile bool         msgPending = false;
volatile AlarmMessage pendingMsg;

unsigned long lastTimeSyncMs    = 0;
unsigned long lastAlarmResendMs = 0;
unsigned long lastDisplayMs     = 0;
int           lastCheckedMinute = -1;
int           ntpSyncCount      = 0;

WebServer server(80);

// ── Sprite for flicker-free display ──────────────────────────────────────────
M5Canvas canvas(&M5.Lcd);

// ═══════════════════════════════════════════════════════════════════════════
// Forward declarations
// ═══════════════════════════════════════════════════════════════════════════
void drawDisplay();
void sendAlarmStart();
void setRtcFromNTP();

// Snooze delay depends on current cycle (last round = longer)
unsigned long getSnoozeDelay() {
    return (snoozeCount >= SNOOZE_MAX_CYCLES - 1)
         ? SNOOZE_DELAY_LONG_MS : SNOOZE_DELAY_SHORT_MS;
}

// ═══════════════════════════════════════════════════════════════════════════
// ESP-NOW callbacks (ISR context)
// ═══════════════════════════════════════════════════════════════════════════
void IRAM_ATTR onDataSent(const uint8_t *mac, esp_now_send_status_t status) {}

void IRAM_ATTR onDataRecv(const uint8_t *mac_addr,
                          const uint8_t *data, int len) {
    if (len == sizeof(AlarmMessage) && !msgPending) {
        memcpy((void*)&pendingMsg, data, sizeof(AlarmMessage));
        msgPending = true;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// NVS
// ═══════════════════════════════════════════════════════════════════════════
void loadAlarmFromNVS() {
    Preferences prefs;
    prefs.begin("alarm", true);
    alarmHour    = (uint8_t)prefs.getUInt("hour",    7);
    alarmMinute  = (uint8_t)prefs.getUInt("minute",  0);
    alarmEnabled = prefs.getBool("enabled", true);
    prefs.end();
}

void saveAlarmToNVS() {
    Preferences prefs;
    prefs.begin("alarm", false);
    prefs.putUInt("hour",    alarmHour);
    prefs.putUInt("minute",  alarmMinute);
    prefs.putBool("enabled", alarmEnabled);
    prefs.end();
}

// ═══════════════════════════════════════════════════════════════════════════
// Set RTC from NTP (M5StickC Plus 2 / M5Unified API)
// ═══════════════════════════════════════════════════════════════════════════
void setRtcFromNTP() {
    struct tm ti;
    if (!getLocalTime(&ti)) {
        Serial.println("[KITCHEN] NTP: getLocalTime failed.");
        return;
    }
    m5::rtc_time_t rtcTime;
    rtcTime.hours   = (int8_t)ti.tm_hour;
    rtcTime.minutes = (int8_t)ti.tm_min;
    rtcTime.seconds = (int8_t)ti.tm_sec;

    m5::rtc_date_t rtcDate;
    rtcDate.year    = (int16_t)(ti.tm_year + 1900);
    rtcDate.month   = (int8_t)(ti.tm_mon + 1);
    rtcDate.date    = (int8_t)ti.tm_mday;
    rtcDate.weekDay = (int8_t)ti.tm_wday;

    M5.Rtc.setTime(&rtcTime);
    M5.Rtc.setDate(&rtcDate);

    Serial.printf("[KITCHEN] RTC set: %04d-%02d-%02d %02d:%02d:%02d\n",
        rtcDate.year, rtcDate.month, rtcDate.date,
        rtcTime.hours, rtcTime.minutes, rtcTime.seconds);
}

// ═══════════════════════════════════════════════════════════════════════════
// Send ESP-NOW messages
// ═══════════════════════════════════════════════════════════════════════════
void sendTimeSync() {
    AlarmMessage msg;
    msg.type   = MSG_TIME_SYNC;
    msg.epoch  = (uint32_t)time(NULL);  // time(NULL) = always correct UTC epoch
    msg.alarmH = 0;
    msg.alarmM = 0;
    msg.msgId  = ++msgCounter;
    esp_now_send(peerMac, (uint8_t*)&msg, sizeof(msg));
}

void sendAlarmSet() {
    AlarmMessage msg;
    msg.type   = MSG_ALARM_SET;
    msg.epoch  = 0;
    msg.alarmH = alarmHour;
    msg.alarmM = alarmMinute;
    msg.msgId  = ++msgCounter;
    esp_now_send(peerMac, (uint8_t*)&msg, sizeof(msg));
}

void sendAlarmDisable() {
    AlarmMessage msg;
    msg.type   = MSG_ALARM_DISABLE;
    msg.epoch  = 0;
    msg.alarmH = 0xFF;
    msg.alarmM = 0xFF;
    msg.msgId  = ++msgCounter;
    esp_now_send(peerMac, (uint8_t*)&msg, sizeof(msg));
}

void sendAlarmStart() {
    AlarmMessage msg;
    msg.type   = MSG_ALARM_START;
    msg.epoch  = 0;
    msg.alarmH = alarmHour;
    msg.alarmM = alarmMinute;
    msg.msgId  = ++msgCounter;
    esp_now_send(peerMac, (uint8_t*)&msg, sizeof(msg));
    lastAlarmResendMs = millis();
}

void sendAlarmStop() {
    AlarmMessage msg;
    msg.type   = MSG_ALARM_STOP;
    msg.epoch  = 0;
    msg.alarmH = 0;
    msg.alarmM = 0;
    msg.msgId  = ++msgCounter;
    esp_now_send(peerMac, (uint8_t*)&msg, sizeof(msg));
}

// ═══════════════════════════════════════════════════════════════════════════
// Check if confirmation is needed: only if alarm is <= CONFIRM_THRESHOLD_H away
// Returns: true = confirmation required, false = apply directly
// ═══════════════════════════════════════════════════════════════════════════
bool needsConfirmation(uint8_t targetH, uint8_t targetM) {
    // If no alarm is currently set → never require confirmation
    if (!alarmEnabled) return false;

    struct tm ti;
    if (!getLocalTime(&ti)) return true;  // safe default: require confirmation if time unknown

    int nowMin    = ti.tm_hour * 60 + ti.tm_min;
    int alarmMin  = targetH * 60 + targetM;
    int diffMin   = alarmMin - nowMin;
    if (diffMin <= 0) diffMin += 24 * 60;  // next day

    return (diffMin <= CONFIRM_THRESHOLD_H * 60);
}

// ═══════════════════════════════════════════════════════════════════════════
// Execute confirmed action (after BtnA press)
// ═══════════════════════════════════════════════════════════════════════════
void executeConfirmedAction() {
    if (confirmPending == CONFIRM_SET) {
        alarmHour    = confirmHour;
        alarmMinute  = confirmMinute;
        alarmEnabled = true;
        saveAlarmToNVS();
        sendAlarmSet();
        Serial.printf("[KITCHEN] ✓ Alarm set (BtnA): %02d:%02d\n", alarmHour, alarmMinute);
    } else if (confirmPending == CONFIRM_DISABLE) {
        alarmEnabled = false;
        saveAlarmToNVS();
        sendAlarmDisable();
        Serial.println("[KITCHEN] ✓ Alarm disabled (BtnA).");
    }
    confirmDone   = true;
    confirmDoneMs = millis();
    // confirmPending stays set — for /confirm-status feedback (cleared after TTL)
}

// ═══════════════════════════════════════════════════════════════════════════
// Web server handlers
// ═══════════════════════════════════════════════════════════════════════════
void handleRoot() {
    server.send(200, "text/html", WEBUI);
}

// POST /request-confirm  body: action=set&hour=X&minute=Y  or  action=disable
void handleRequestConfirm() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    if (webLocked) {
        server.send(423, "text/plain", "Locked — dismiss snooze cycles first");
        return;
    }
    if (alarmActive) {
        server.send(409, "text/plain", "Alarm is ringing — press BtnA first");
        return;
    }
    if (confirmPending != CONFIRM_NONE && !confirmDone) {
        server.send(409, "text/plain", "Confirmation already in progress");
        return;
    }

    String action = server.arg("action");
    if (action == "set") {
        if (!server.hasArg("hour") || !server.hasArg("minute")) {
            server.send(400, "text/plain", "hour/minute missing");
            return;
        }
        int h = server.arg("hour").toInt();
        int m = server.arg("minute").toInt();
        if (h < 0 || h > 23 || m < 0 || m > 59) {
            server.send(400, "text/plain", "Invalid time");
            return;
        }

        // Confirmation only required if CURRENTLY SET alarm is due in <= 5h
        if (needsConfirmation(alarmHour, alarmMinute)) {
            confirmPending   = CONFIRM_SET;
            confirmHour      = (uint8_t)h;
            confirmMinute    = (uint8_t)m;
            confirmRequestMs = millis();
            confirmDone      = false;
            Serial.printf("[KITCHEN] Confirmation requested: alarm %02d:%02d (<=5h)\n", h, m);
        } else {
            // Apply directly — no confirmation needed
            alarmHour    = (uint8_t)h;
            alarmMinute  = (uint8_t)m;
            alarmEnabled = true;
            saveAlarmToNVS();
            sendAlarmSet();
            // Set confirmDone so the web UI shows success
            confirmPending = CONFIRM_SET;
            confirmHour    = (uint8_t)h;
            confirmMinute  = (uint8_t)m;
            confirmDone    = true;
            confirmDoneMs  = millis();
            Serial.printf("[KITCHEN] Alarm set directly: %02d:%02d (>5h or no alarm active)\n", h, m);
        }
    } else if (action == "disable") {
        // Confirmation only required if current alarm is due in <= 5h
        if (needsConfirmation(alarmHour, alarmMinute)) {
            confirmPending   = CONFIRM_DISABLE;
            confirmHour      = 0;
            confirmMinute    = 0;
            confirmRequestMs = millis();
            confirmDone      = false;
            Serial.println("[KITCHEN] Confirmation requested: disable alarm (<=5h)");
        } else {
            // Apply directly — no confirmation needed
            alarmEnabled = false;
            saveAlarmToNVS();
            sendAlarmDisable();
            confirmPending = CONFIRM_DISABLE;
            confirmHour    = 0;
            confirmMinute  = 0;
            confirmDone    = true;
            confirmDoneMs  = millis();
            Serial.println("[KITCHEN] Alarm disabled directly (>5h or no alarm active).");
        }
    } else {
        server.send(400, "text/plain", "Invalid action");
        return;
    }

    drawDisplay();
    // "OK:direct" = applied immediately, "OK:confirm" = confirmation required
    server.send(200, "text/plain", confirmDone ? "OK:direct" : "OK:confirm");
}

// GET /confirm-status  → JSON with current confirmation state
void handleConfirmStatus() {
    char json[192];
    if (confirmDone) {
        const char* a = (confirmPending == CONFIRM_SET) ? "set" : "disable";
        snprintf(json, sizeof(json),
            "{\"state\":\"confirmed\",\"action\":\"%s\",\"hour\":%d,\"minute\":%d}",
            a, confirmHour, confirmMinute);
    } else if (confirmPending != CONFIRM_NONE) {
        unsigned long elapsed = millis() - confirmRequestMs;
        if (elapsed >= CONFIRM_TIMEOUT_MS) {
            confirmPending = CONFIRM_NONE;
            drawDisplay();
            snprintf(json, sizeof(json), "{\"state\":\"timeout\"}");
        } else {
            int rem = (int)((CONFIRM_TIMEOUT_MS - elapsed) / 1000);
            const char* a = (confirmPending == CONFIRM_SET) ? "set" : "disable";
            snprintf(json, sizeof(json),
                "{\"state\":\"waiting\",\"action\":\"%s\",\"hour\":%d,\"minute\":%d,\"remaining\":%d}",
                a, confirmHour, confirmMinute, rem);
        }
    } else {
        snprintf(json, sizeof(json), "{\"state\":\"none\"}");
    }
    server.send(200, "application/json", json);
}

// POST /cancel-confirm  → cancel a pending confirmation
void handleCancelConfirm() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    confirmPending = CONFIRM_NONE;
    confirmDone    = false;
    drawDisplay();
    Serial.println("[KITCHEN] Confirmation cancelled.");
    server.send(200, "text/plain", "OK");
}

void handleTestAlarm() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    if (webLocked) {
        server.send(423, "text/plain", "Locked — dismiss snooze cycles first");
        return;
    }
    alarmActive      = true;
    alarmAckReceived = false;
    snoozeActive     = false;  // cancel any active snooze cycle
    snoozeCount      = 0;      // reset snooze counter
    kitchenBuzzing   = false;  // test alarm: bedroom only
    sendAlarmStart();
    Serial.println("[KITCHEN] Web: test alarm triggered.");
    server.send(200, "text/plain", "OK");
    drawDisplay();
}

void handleStatus() {
    struct tm ti;
    char timeBuf[6] = "--:--";
    if (getLocalTime(&ti)) {
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", ti.tm_hour, ti.tm_min);
    }
    int snoozeRemaining = 0;
    if (snoozeActive) {
        unsigned long delay = getSnoozeDelay();
        unsigned long elapsed = millis() - snoozeStartMs;
        snoozeRemaining = (delay > elapsed)
                        ? (int)((delay - elapsed) / 1000) : 0;
    }
    char json[320];
    snprintf(json, sizeof(json),
        "{\"hour\":%d,\"minute\":%d,\"enabled\":%s,\"active\":%s,"
        "\"snooze\":%s,\"snoozeRemaining\":%d,"
        "\"webLocked\":%s,\"snoozeCount\":%d,\"snoozeMax\":%d,"
        "\"ip\":\"%s\",\"time\":\"%s\"}",
        alarmHour, alarmMinute,
        alarmEnabled ? "true" : "false",
        alarmActive  ? "true" : "false",
        snoozeActive ? "true" : "false",
        snoozeRemaining,
        webLocked    ? "true" : "false",
        snoozeCount, SNOOZE_MAX_CYCLES,
        WiFi.localIP().toString().c_str(),
        timeBuf
    );
    server.send(200, "application/json", json);
}

// ═══════════════════════════════════════════════════════════════════════════
// Draw display
// ═══════════════════════════════════════════════════════════════════════════
void drawDisplay() {
    struct tm ti;
    char timeBuf[6] = "--:--";
    if (getLocalTime(&ti)) {
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", ti.tm_hour, ti.tm_min);
    }

    canvas.fillScreen(BLACK);

    if (alarmActive) {
        M5.Lcd.setBrightness(BRIGHTNESS_ALARM);

        canvas.setTextColor(RED, BLACK);
        canvas.setTextSize(2);
        canvas.setCursor(8, 8);
        canvas.print(kitchenBuzzing ? "SNOOZE ALARM!" : "ALARM RINGING!");

        canvas.setTextColor(WHITE, BLACK);
        canvas.setTextSize(2);
        canvas.setCursor(8, 42);
        canvas.print(">> Press BtnA");

        canvas.setTextSize(3);
        canvas.setCursor(38, 82);
        canvas.print(timeBuf);

    } else if (snoozeActive) {
        M5.Lcd.setBrightness(BRIGHTNESS_ALARM);

        canvas.setTextColor(0xFD20, BLACK);  // Orange
        canvas.setTextSize(2);
        canvas.setCursor(8, 8);
        char snoozeTitle[20];
        snprintf(snoozeTitle, sizeof(snoozeTitle), "SNOOZE %d/%d", snoozeCount + 1, SNOOZE_MAX_CYCLES);
        canvas.print(snoozeTitle);

        unsigned long delay = getSnoozeDelay();
        unsigned long elapsed = millis() - snoozeStartMs;
        int remaining = (delay > elapsed)
                      ? (int)((delay - elapsed) / 1000) : 0;
        char snoozeBuf[24];
        snprintf(snoozeBuf, sizeof(snoozeBuf), "Next: %ds", remaining);
        canvas.setTextColor(WHITE, BLACK);
        canvas.setTextSize(2);
        canvas.setCursor(8, 42);
        canvas.print(snoozeBuf);

        canvas.setTextSize(3);
        canvas.setCursor(38, 82);
        canvas.print(timeBuf);

    } else if (confirmPending != CONFIRM_NONE && !confirmDone) {
        M5.Lcd.setBrightness(BRIGHTNESS_ALARM);

        canvas.setTextColor(0xFD20, BLACK);  // Orange
        canvas.setTextSize(2);
        canvas.setCursor(8, 6);
        canvas.print("Press BtnA!");

        canvas.setTextColor(WHITE, BLACK);
        canvas.setTextSize(1);
        canvas.setCursor(8, 34);
        if (confirmPending == CONFIRM_SET) {
            char buf[26];
            snprintf(buf, sizeof(buf), "Set alarm %02d:%02d", confirmHour, confirmMinute);
            canvas.print(buf);
        } else {
            canvas.print("Disable alarm");
        }

        unsigned long elapsed = millis() - confirmRequestMs;
        int rem = (int)((CONFIRM_TIMEOUT_MS - elapsed) / 1000);
        if (rem < 0) rem = 0;
        char remBuf[14];
        snprintf(remBuf, sizeof(remBuf), "Timeout: %2ds", rem);
        canvas.setCursor(8, 50);
        canvas.print(remBuf);

        canvas.setTextColor(0x4208, BLACK);  // Dark grey
        canvas.setTextSize(3);
        canvas.setCursor(38, 88);
        canvas.print(timeBuf);

    } else {
        M5.Lcd.setBrightness(BRIGHTNESS_NORMAL);

        canvas.setTextColor(WHITE, BLACK);
        canvas.setTextSize(4);
        canvas.setCursor(22, 6);
        canvas.print(timeBuf);

        if (alarmEnabled) {
            char alarmBuf[9];
            snprintf(alarmBuf, sizeof(alarmBuf), "WK%02d:%02d", alarmHour, alarmMinute);
            canvas.setTextColor(0xFD20, BLACK);  // Orange
            canvas.setTextSize(2);
            canvas.setCursor(30, 60);
            canvas.print(alarmBuf);

            canvas.setTextSize(1);
            canvas.setTextColor(0x07E0, BLACK);  // Green
            canvas.setCursor(5, 97);
            canvas.print("STATUS: ARMED");
        } else {
            canvas.setTextColor(0x8410, BLACK);  // Grau
            canvas.setTextSize(2);
            canvas.setCursor(22, 60);
            canvas.print("NO ALARM");

            canvas.setTextSize(1);
            canvas.setTextColor(0x8410, BLACK);
            canvas.setCursor(5, 97);
            canvas.print("STATUS: DISABLED");
        }

        canvas.setTextColor(0x4208, BLACK);  // Dark grey
        canvas.setCursor(5, 114);
        if (WiFi.status() == WL_CONNECTED) {
            canvas.print("alarm.local");
        } else {
            canvas.setTextColor(RED, BLACK);
            canvas.print("WiFi offline");
        }
    }

    canvas.pushSprite(0, 0);  // atomic push → no flicker
}

// ═══════════════════════════════════════════════════════════════════════════
// setup()
// ═══════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(200);
    auto cfg = M5.config();
    M5.begin(cfg);

    // Prepare kitchen buzzer (internal piezo) for snooze alarm
    M5.Speaker.setVolume(255);

    M5.Lcd.setRotation(3);
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setBrightness(BRIGHTNESS_NORMAL);
    canvas.createSprite(M5.Lcd.width(), M5.Lcd.height());
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(WHITE, BLACK);

    // ── Boot screen ───────────────────────────────────────────────────────
    auto showStatus = [](int y, const char* msg, uint16_t color = WHITE) {
        M5.Lcd.setTextColor(color, BLACK);
        M5.Lcd.setCursor(5, y);
        M5.Lcd.print(msg);
    };

    showStatus(5, "Bulletproof Wakeup");
    showStatus(20, "Starting...");

    // ── Connect WiFi ──────────────────────────────────────────────────────
    showStatus(35, "WiFi...");
    WiFi.persistent(false);   // gecachte IP/Credentials NICHT in Flash schreiben
    WiFi.disconnect(true);    // alten DHCP-Lease vergessen → saubere Verbindung
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int wifiRetry = 40; // 20 seconds
    while (WiFi.status() != WL_CONNECTED && wifiRetry > 0) {
        delay(500);
        wifiRetry--;
    }

    if (WiFi.status() == WL_CONNECTED) {
        showStatus(35, "WiFi OK          ", 0x07E0);
        Serial.printf("[KITCHEN] WiFi: %s  IP: %s  channel: %d\n",
            WIFI_SSID, WiFi.localIP().toString().c_str(), WiFi.channel());
        Serial.printf("[KITCHEN] Router channel: %d — check ESPNOW_CHANNEL in bedroom/include/config.h!\n",
            WiFi.channel());
    } else {
        showStatus(35, "WiFi FAILED!     ", RED);
        Serial.println("[KITCHEN] WiFi connection failed.");
    }

    // ── NTP time synchronization ──────────────────────────────────────────
    if (WiFi.status() == WL_CONNECTED) {
        showStatus(50, "NTP sync...");
        configTime(0, 0, NTP_SERVER);
        setenv("TZ", TIMEZONE, 1);
        tzset();

        struct tm ti;
        int ntpRetry = 20;
        while (ntpRetry > 0 && !getLocalTime(&ti)) {
            delay(500);
            ntpRetry--;
        }
        if (ti.tm_year > 70) {
            setRtcFromNTP();
            showStatus(50, "NTP OK           ", 0x07E0);
        } else {
            showStatus(50, "NTP timeout      ", 0xFD20);
            Serial.println("[KITCHEN] NTP sync failed.");
        }
    }

    // ── Initialize ESP-NOW ────────────────────────────────────────────────
    showStatus(65, "ESP-NOW...");

    if (esp_now_init() != ESP_OK) {
        showStatus(65, "ESP-NOW FAILED!  ", RED);
        Serial.println("[KITCHEN] ESP-NOW init FAILED!");
    } else {
        uint8_t ch = (WiFi.status() == WL_CONNECTED) ? (uint8_t)WiFi.channel() : 1;
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[KITCHEN] ESP-NOW channel: %d\n", ch);

        esp_now_register_send_cb(onDataSent);
        esp_now_register_recv_cb(onDataRecv);

        bool macIsZero = true;
        for (int i = 0; i < 6; i++) {
            if (peerMac[i] != 0x00) { macIsZero = false; break; }
        }

        if (!macIsZero) {
            esp_now_peer_info_t peer = {};
            memcpy(peer.peer_addr, peerMac, 6);
            peer.channel = 0;
            peer.encrypt = false;
            peer.ifidx   = WIFI_IF_STA;

            if (esp_now_add_peer(&peer) != ESP_OK) {
                Serial.println("[KITCHEN] ✗ Failed to register peer!");
            } else {
                showStatus(65, "ESP-NOW OK       ", 0x07E0);
                Serial.println("[KITCHEN] ✓ Bedroom peer registered.");
            }
        } else {
            showStatus(65, "ESP-NOW: MAC?    ", 0xFD20);
        }
    }

    // ── Load alarm time from NVS ──────────────────────────────────────────
    loadAlarmFromNVS();

    // ── Start web server ──────────────────────────────────────────────────
    server.on("/",                HTTP_GET,  handleRoot);
    server.on("/request-confirm", HTTP_POST, handleRequestConfirm);
    server.on("/confirm-status",  HTTP_GET,  handleConfirmStatus);
    server.on("/cancel-confirm",  HTTP_POST, handleCancelConfirm);
    server.on("/test-alarm",      HTTP_POST, handleTestAlarm);
    server.on("/status",          HTTP_GET,  handleStatus);
    server.begin();

    // Start mDNS → http://alarm.local
    if (WiFi.status() == WL_CONNECTED) {
        if (MDNS.begin(MDNS_HOSTNAME)) {
            MDNS.addService("http", "tcp", 80);
            Serial.println("[KITCHEN] mDNS: http://alarm.local");
        }
    }
    showStatus(80, "WebServer OK     ", 0x07E0);

    // ── Start watchdog ────────────────────────────────────────────────────
    esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
    esp_task_wdt_add(NULL);

    // ── Print status ───────────────────────────────────────────────────────
    Serial.printf("[KITCHEN] ═══════════════════════════════\n");
    Serial.printf("[KITCHEN] MAC:   %s\n", WiFi.macAddress().c_str());
    Serial.printf("[KITCHEN] IP:    %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[KITCHEN] Alarm: %02d:%02d (%s)\n",
        alarmHour, alarmMinute, alarmEnabled ? "active" : "disabled");
    Serial.println("[KITCHEN] ✓ Ready.");

    delay(1500);
    drawDisplay();
}

// ═══════════════════════════════════════════════════════════════════════════
// loop()
// ═══════════════════════════════════════════════════════════════════════════
void loop() {
    esp_task_wdt_reset();
    M5.update();
    server.handleClient();

    unsigned long now = millis();

    // ── Process incoming ESP-NOW messages ─────────────────────────────────
    if (msgPending) {
        AlarmMessage msg;
        memcpy(&msg, (void*)&pendingMsg, sizeof(AlarmMessage));
        msgPending = false;

        if (msg.type == MSG_ACK) {
            alarmAckReceived = true;
            Serial.println("[KITCHEN] ACK received.");
        }
    }

    // ── BtnA ──────────────────────────────────────────────────────────────
    if (M5.BtnA.wasPressed()) {
        if (alarmActive) {
            // Priority 1: stop the active alarm
            alarmActive      = false;
            alarmAckReceived = false;
            confirmPending   = CONFIRM_NONE;  // discard any pending confirmation
            confirmDone      = false;
            // Stop kitchen buzzer if active
            if (kitchenBuzzing) {
                M5.Speaker.stop();
                kitchenBuzzing = false;
                kitchenPulseOn = false;
            }
            sendAlarmStop();
            // Start snooze: re-trigger after delay (max 3×)
            if (snoozeCount < SNOOZE_MAX_CYCLES) {
                snoozeActive  = true;
                snoozeStartMs = millis();
                webLocked     = true;  // lock web UI until all snooze cycles are done
                Serial.printf("[KITCHEN] BtnA: alarm stopped → snooze %d/%d in %lus. Web LOCKED.\n",
                              snoozeCount + 1, SNOOZE_MAX_CYCLES, getSnoozeDelay() / 1000);
            } else {
                snoozeActive = false;
                webLocked    = false;  // all snooze cycles done → unlock web UI
                Serial.println("[KITCHEN] BtnA: alarm stopped → snooze cycles exhausted. Web UNLOCKED.");
            }
            drawDisplay();
        } else if (confirmPending != CONFIRM_NONE && !confirmDone) {
            // Priority 2: confirm pending setting
            executeConfirmedAction();
            drawDisplay();
        }
    }

    // ── TIME_SYNC every 60 seconds ───────────────────────────────────────
    if (now - lastTimeSyncMs >= TIME_SYNC_MS) {
        lastTimeSyncMs = now;
        sendTimeSync();

        if (++ntpSyncCount >= NTP_RESYNC_CYCLES) {
            ntpSyncCount = 0;
            if (WiFi.status() == WL_CONNECTED) {
                setRtcFromNTP();
                Serial.println("[KITCHEN] Hourly NTP re-sync.");
            }
        }
    }

    // ── Alarm time check (only when alarm is enabled) ─────────────────────
    struct tm ti;
    if (alarmEnabled && getLocalTime(&ti)) {
        if (ti.tm_min != lastCheckedMinute) {
            lastCheckedMinute = ti.tm_min;
            if (ti.tm_hour == alarmHour && ti.tm_min == alarmMinute && !alarmActive) {
                alarmActive      = true;
                alarmAckReceived = false;
                snoozeActive     = false;  // cancel any active snooze cycle
                snoozeCount      = 0;      // reset snooze counter
                kitchenBuzzing   = false;  // initial trigger: bedroom only
                sendAlarmStart();
                Serial.printf("[KITCHEN] ★ ALARM triggered! %02d:%02d\n",
                              alarmHour, alarmMinute);
                drawDisplay();
            }
        }
    }

    // ── Repeat ALARM_START every 5s until ACK ────────────────────────────
    if (alarmActive && !alarmAckReceived) {
        if (now - lastAlarmResendMs >= ALARM_RESEND_MS) {
            sendAlarmStart();
            Serial.println("[KITCHEN] ALARM_START resent (no ACK yet)...");
        }
    }

    // ── Snooze: re-trigger after delay (60s / 60s / 120s) ────────────
    if (snoozeActive && !alarmActive && (now - snoozeStartMs >= getSnoozeDelay())) {
        snoozeActive     = false;
        snoozeCount++;
        alarmActive      = true;
        alarmAckReceived = false;
        kitchenBuzzing   = true;
        kitchenPulseOn   = false;
        kitchenPulseMs   = now;
        sendAlarmStart();
        Serial.printf("[KITCHEN] ★ SNOOZE %d/%d: alarm again! (bedroom + kitchen)\n",
                      snoozeCount, SNOOZE_MAX_CYCLES);
        drawDisplay();
    }

    // ── Pulse kitchen buzzer (600ms on / 100ms off) ────────────────────
    if (alarmActive && kitchenBuzzing) {
        if (!kitchenPulseOn && (now - kitchenPulseMs >= KITCHEN_PULSE_OFF_MS)) {
            M5.Speaker.tone(KITCHEN_TONE_HZ);
            kitchenPulseOn = true;
            kitchenPulseMs = now;
        } else if (kitchenPulseOn && (now - kitchenPulseMs >= KITCHEN_PULSE_ON_MS)) {
            M5.Speaker.stop();
            kitchenPulseOn = false;
            kitchenPulseMs = now;
        }
    }

    // ── Confirmation timeout (fallback if /confirm-status is not polled) ─
    if (confirmPending != CONFIRM_NONE && !confirmDone) {
        if (now - confirmRequestMs >= CONFIRM_TIMEOUT_MS) {
            Serial.println("[KITCHEN] Confirmation timeout.");
            confirmPending = CONFIRM_NONE;
            drawDisplay();
        }
    }

    // ── Confirmation-done TTL: reset after 10s ────────────────────────────
    if (confirmDone && (now - confirmDoneMs >= CONFIRM_DONE_TTL_MS)) {
        confirmDone    = false;
        confirmPending = CONFIRM_NONE;
    }

    // ── Update display every second ───────────────────────────────────────
    if (now - lastDisplayMs >= DISPLAY_UPDATE_MS) {
        lastDisplayMs = now;
        drawDisplay();
    }
}
