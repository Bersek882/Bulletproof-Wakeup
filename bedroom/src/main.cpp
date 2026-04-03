// ═══════════════════════════════════════════════════════════════════════════
// BULLETPROOF WAKEUP — Bedroom (Slave)
// Device: M5StickC Plus 2 (M5Unified — auto-detect)
// Role: Receives ESP-NOW commands from the kitchen device.
//       Makes loud noise. Can ONLY be stopped via the kitchen device.
// ═══════════════════════════════════════════════════════════════════════════

#include <M5Unified.h>
#include <esp_now.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include "config.h"

// ── Pins ─────────────────────────────────────────────────────────────────────
#define INTERNAL_BUZZER_PIN 2    // GPIO2:  Built-in piezo buzzer M5StickC Plus 2
#define SPEAKER_ENABLE_PIN  0    // GPIO0:  PAM8303 enable (HIGH = on)
#define SPEAKER_AUDIO_PIN   26   // GPIO26: PAM8303 audio input (LEDC PWM)
#define BUZZER_PIN          32   // GPIO32: M5Stack Buzzer Unit (PORT.A Grove SIG1)

// ── LEDC channels ─────────────────────────────────────────────────────────────
// ESP32 LEDC timer pairs (HS): Ch0+1=T0, Ch2+3=T1, Ch4+5=T2, Ch6+7=T3
// Different frequencies require different timer pairs!
//   Ch 4 + Ch 5 → Timer 2 → both buzzers at 4000 Hz (same timer, no conflict)
//   Ch 6        → Timer 3 → speaker alone at 1000 Hz
#define INTERNAL_BUZZER_CH  4    // GPIO2  internal buzzer  → Timer 2 @ 4000 Hz
#define BUZZER_LEDC_CH      5    // GPIO32 Buzzer Unit      → Timer 2 @ 4000 Hz
#define SPEAKER_LEDC_CH     6    // GPIO26 Speaker HAT      → Timer 3 @ 1000 Hz (alone!)
#define LEDC_RES_BITS       8    // 8-bit → duty 0–255

// ── Alarm tone ───────────────────────────────────────────────────────────────
#define BUZZER_TONE_HZ      4000 // 4000 Hz — resonant frequency of piezo buzzer
#define SPEAKER_TONE_HZ     1000 // 1000 Hz — optimal frequency for speaker membrane (fuller, louder)
#define ALARM_DUTY          128  // 50% duty cycle — maximum amplitude

// ── Display brightness (0–255) ────────────────────────────────────────────────
#define BRIGHTNESS_DIM      60   // Night mode: dimmed but visible
#define BRIGHTNESS_FULL     255  // Alarm: full brightness

// ── Watchdog ─────────────────────────────────────────────────────────────────
#define WDT_TIMEOUT_SEC     10

// ── Alarm timing (non-blocking) ───────────────────────────────────────────────
#define PULSE_ON_MS         600  // long on = near-continuous noise
#define PULSE_OFF_MS        100  // short pause = barely any relief
#define BLINK_INTERVAL_MS   500
#define DISPLAY_UPDATE_MS   1000

// ═══════════════════════════════════════════════════════════════════════════
// Message protocol (identical to kitchen/src/main.cpp)
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

// ── Global state variables ────────────────────────────────────────────────────
uint8_t peerMac[]    = PEER_MAC;
uint8_t alarmHour    = 7;
uint8_t alarmMinute  = 0;
bool    alarmEnabled = true;

volatile bool         msgPending = false;
volatile AlarmMessage pendingMsg;

bool alarmActive       = false;
bool pulseOn           = false;
bool displayBlinkState = false;

unsigned long pulseStartMs        = 0;
unsigned long lastBlinkMs         = 0;
unsigned long lastDisplayUpdateMs = 0;

// ── Sprite for flicker-free display ──────────────────────────────────────────
M5Canvas canvas(&M5.Display);

// ═══════════════════════════════════════════════════════════════════════════
// Sound — 3 sources, all via LEDC (M5Unified Speaker fully disabled)
//   GPIO2  = built-in piezo buzzer M5StickC Plus 1.1
//   GPIO26 = PAM8303 Speaker HAT (amplifier, 3W)
//   GPIO32 = M5Stack Buzzer Unit (external, Grove)
// ═══════════════════════════════════════════════════════════════════════════
void setupSound() {
    // PAM8303 Enable: LOW = muted
    pinMode(SPEAKER_ENABLE_PIN, OUTPUT);
    digitalWrite(SPEAKER_ENABLE_PIN, LOW);

    // Ch4+Ch5 → Timer 2 @ 4000 Hz (both buzzers, same timer = no conflict)
    ledcSetup(INTERNAL_BUZZER_CH, BUZZER_TONE_HZ, LEDC_RES_BITS);  // Ch4, Timer2
    ledcAttachPin(INTERNAL_BUZZER_PIN, INTERNAL_BUZZER_CH);
    ledcWrite(INTERNAL_BUZZER_CH, 0);

    ledcSetup(BUZZER_LEDC_CH, BUZZER_TONE_HZ, LEDC_RES_BITS);      // Ch5, Timer2
    ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CH);
    ledcWrite(BUZZER_LEDC_CH, 0);

    // Ch6 → Timer 3 @ 1000 Hz (speaker alone on its own timer — NO override!)
    ledcSetup(SPEAKER_LEDC_CH, SPEAKER_TONE_HZ, LEDC_RES_BITS);    // Ch6, Timer3
    ledcAttachPin(SPEAKER_AUDIO_PIN, SPEAKER_LEDC_CH);
    ledcWrite(SPEAKER_LEDC_CH, 0);
}

void soundOn() {
    ledcWrite(INTERNAL_BUZZER_CH, ALARM_DUTY);    // internal piezo
    digitalWrite(SPEAKER_ENABLE_PIN, HIGH);        // enable PAM8303
    ledcWrite(SPEAKER_LEDC_CH, ALARM_DUTY);        // Speaker HAT (3W)
    ledcWrite(BUZZER_LEDC_CH,  ALARM_DUTY);        // Buzzer Unit
}

void soundOff() {
    ledcWrite(INTERNAL_BUZZER_CH, 0);
    ledcWrite(SPEAKER_LEDC_CH, 0);
    ledcWrite(BUZZER_LEDC_CH,  0);
    digitalWrite(SPEAKER_ENABLE_PIN, LOW);         // disable PAM8303 (no noise)
}

// ═══════════════════════════════════════════════════════════════════════════
// NVS — persist alarm time across reboots
// ═══════════════════════════════════════════════════════════════════════════
void loadAlarmFromNVS() {
    Preferences prefs;
    prefs.begin("alarm", true);
    alarmHour    = (uint8_t)prefs.getUInt("hour",   7);
    alarmMinute  = (uint8_t)prefs.getUInt("minute", 0);
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
// ESP-NOW — send ACK back
// ═══════════════════════════════════════════════════════════════════════════
void sendAck(uint8_t msgId) {
    AlarmMessage ack;
    ack.type   = MSG_ACK;
    ack.epoch  = 0;
    ack.alarmH = 0;
    ack.alarmM = 0;
    ack.msgId  = msgId;
    esp_now_send(peerMac, (uint8_t*)&ack, sizeof(ack));
}

// ═══════════════════════════════════════════════════════════════════════════
// ESP-NOW callbacks (ISR context → memcpy + flag only)
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
// Trigger / stop alarm
// ═══════════════════════════════════════════════════════════════════════════
void triggerAlarm() {
    alarmActive  = true;
    pulseOn      = false;
    pulseStartMs = millis();
    M5.Display.setBrightness(BRIGHTNESS_FULL);
    Serial.println("[BEDROOM] ALARM ACTIVE!");
}

void stopAlarm() {
    alarmActive = false;
    soundOff();
    displayBlinkState = false;
    M5.Display.setBrightness(BRIGHTNESS_DIM);
    Serial.println("[BEDROOM] Alarm stopped.");
}

// ═══════════════════════════════════════════════════════════════════════════
// Draw display — via sprite (no flicker!)
// ═══════════════════════════════════════════════════════════════════════════
void drawDisplay() {
    m5::rtc_time_t t;
    M5.Rtc.getTime(&t);
    char timeBuf[6];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", t.hours, t.minutes);

    canvas.fillScreen(BLACK);

    if (alarmActive) {
        uint16_t col = displayBlinkState ? RED : WHITE;
        canvas.setTextColor(col, BLACK);
        canvas.setTextSize(3);
        canvas.setCursor(8, 18);
        canvas.print("WAKE UP!");

        canvas.setTextColor(WHITE, BLACK);
        canvas.setTextSize(4);
        canvas.setCursor(28, 78);
        canvas.print(timeBuf);
    } else {
        canvas.setTextColor(WHITE, BLACK);
        canvas.setTextSize(5);
        canvas.setCursor(14, 16);
        canvas.print(timeBuf);

        if (alarmEnabled) {
            char alarmBuf[9];
            snprintf(alarmBuf, sizeof(alarmBuf), "AL%02d:%02d", alarmHour, alarmMinute);
            canvas.setTextColor(0xFD20, BLACK);  // Orange
            canvas.setTextSize(2);
            canvas.setCursor(46, 95);
            canvas.print(alarmBuf);
        } else {
            canvas.setTextColor(0x8410, BLACK);  // Grau
            canvas.setTextSize(2);
            canvas.setCursor(30, 95);
            canvas.print("NO ALARM");
        }
    }

    canvas.pushSprite(0, 0);  // atomic push → no flicker
}

// ═══════════════════════════════════════════════════════════════════════════
// Boot check: was the alarm missed during a reboot?
// ═══════════════════════════════════════════════════════════════════════════
void checkBootAlarm() {
    if (!alarmEnabled) return;  // skip boot check if alarm is disabled

    m5::rtc_time_t t;
    M5.Rtc.getTime(&t);
    int nowMin   = t.hours * 60 + t.minutes;
    int alarmMin = alarmHour * 60 + alarmMinute;
    int diff     = nowMin - alarmMin;
    if (diff < 0) diff += 24 * 60;
    if (diff >= 0 && diff < 10) {
        Serial.printf("[BEDROOM] Boot check: alarm %02d:%02d missed → triggering immediately!\n",
                      alarmHour, alarmMinute);
        triggerAlarm();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// setup()
// ═══════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(200);

    // Fully disable M5Unified Speaker — we drive GPIO2/26/32 directly via LEDC
    // Prevents conflicts (crackling) between M5Unified task and LEDC on GPIO26
    {
        auto spkCfg = M5.Speaker.config();
        spkCfg.pin_data_out = -1;
        M5.Speaker.config(spkCfg);
    }
    auto cfg = M5.config();
    M5.begin(cfg);

    // Timezone must be set AFTER M5.begin()
    setenv("TZ", TIMEZONE, 1);
    tzset();

    // Display: landscape, black, dimmed
    M5.Display.setRotation(3);
    M5.Display.fillScreen(BLACK);
    M5.Display.setBrightness(BRIGHTNESS_DIM);

    // Sprite for flicker-free rendering
    canvas.createSprite(M5.Display.width(), M5.Display.height());

    // Sound setup: Speaker HAT (GPIO26) + Buzzer Unit (GPIO32)
    setupSound();

    // Load alarm time from flash
    loadAlarmFromNVS();

    // Print own MAC address
    Serial.printf("[BEDROOM] ═══════════════════════════════\n");
    Serial.printf("[BEDROOM] MAC: %s\n", WiFi.macAddress().c_str());
    Serial.printf("[BEDROOM] Alarm: %02d:%02d (%s)\n",
        alarmHour, alarmMinute, alarmEnabled ? "active" : "disabled");
    Serial.printf("[BEDROOM] ESP-NOW channel: %d\n", ESPNOW_CHANNEL);

    // Validate peer MAC
    bool macIsZero = true;
    for (int i = 0; i < 6; i++) {
        if (peerMac[i] != 0x00) { macIsZero = false; break; }
    }
    if (macIsZero) {
        Serial.println("[BEDROOM] ⚠ WARNING: peer MAC is all zeros!");
    }

    // ESP-NOW
    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[BEDROOM] ✗ ESP-NOW init FAILED!");
    } else {
        esp_now_register_send_cb(onDataSent);
        esp_now_register_recv_cb(onDataRecv);

        if (!macIsZero) {
            esp_now_peer_info_t peer = {};
            memcpy(peer.peer_addr, peerMac, 6);
            peer.channel = 0;
            peer.encrypt = false;
            if (esp_now_add_peer(&peer) != ESP_OK) {
                Serial.println("[BEDROOM] ✗ Failed to register peer!");
            } else {
                Serial.println("[BEDROOM] ✓ Kitchen peer registered.");
            }
        }
    }

    // Watchdog
    esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
    esp_task_wdt_add(NULL);

    checkBootAlarm();
    drawDisplay();

    Serial.println("[BEDROOM] ✓ Ready.");
}

// ═══════════════════════════════════════════════════════════════════════════
// loop()
// ═══════════════════════════════════════════════════════════════════════════
void loop() {
    esp_task_wdt_reset();
    M5.update();

    unsigned long now = millis();

    // ── Process incoming ESP-NOW messages ─────────────────────────────────
    if (msgPending) {
        AlarmMessage msg;
        memcpy(&msg, (void*)&pendingMsg, sizeof(AlarmMessage));
        msgPending = false;

        switch (msg.type) {

            case MSG_TIME_SYNC: {
                time_t epoch = (time_t)msg.epoch;
                struct tm *ti = localtime(&epoch);
                m5::rtc_time_t rtcTime;
                rtcTime.hours   = (int8_t)ti->tm_hour;
                rtcTime.minutes = (int8_t)ti->tm_min;
                rtcTime.seconds = (int8_t)ti->tm_sec;
                m5::rtc_date_t rtcDate;
                rtcDate.weekDay = (int8_t)ti->tm_wday;
                rtcDate.year    = (int16_t)(ti->tm_year + 1900);
                rtcDate.month   = (int8_t)(ti->tm_mon + 1);
                rtcDate.date    = (int8_t)ti->tm_mday;
                M5.Rtc.setTime(&rtcTime);
                M5.Rtc.setDate(&rtcDate);
                Serial.printf("[BEDROOM] Time sync: %02d:%02d:%02d\n",
                              ti->tm_hour, ti->tm_min, ti->tm_sec);
                break;
            }

            case MSG_ALARM_SET:
                alarmHour    = msg.alarmH;
                alarmMinute  = msg.alarmM;
                alarmEnabled = true;  // setting alarm always enables it
                saveAlarmToNVS();
                sendAck(msg.msgId);
                Serial.printf("[BEDROOM] Alarm set → %02d:%02d (enabled)\n",
                              alarmHour, alarmMinute);
                drawDisplay();
                break;

            case MSG_ALARM_DISABLE:
                alarmEnabled = false;
                saveAlarmToNVS();
                sendAck(msg.msgId);
                Serial.println("[BEDROOM] Alarm disabled.");
                drawDisplay();
                break;

            case MSG_ALARM_START:
                if (!alarmActive) triggerAlarm();
                sendAck(msg.msgId);
                break;

            case MSG_ALARM_STOP:
                stopAlarm();
                sendAck(msg.msgId);
                drawDisplay();
                break;

            default:
                break;
        }
    }

    // ── Alarm: pulse sound (non-blocking) ────────────────────────────────
    if (alarmActive) {
        if (!pulseOn && (now - pulseStartMs >= PULSE_OFF_MS)) {
            soundOn();
            pulseOn      = true;
            pulseStartMs = now;
        } else if (pulseOn && (now - pulseStartMs >= PULSE_ON_MS)) {
            soundOff();
            pulseOn      = false;
            pulseStartMs = now;
        }

        // Blink display
        if (now - lastBlinkMs >= BLINK_INTERVAL_MS) {
            displayBlinkState = !displayBlinkState;
            lastBlinkMs       = now;
            drawDisplay();
        }
    }

    // ── Update display every second (only when no alarm) ─────────────────
    if (!alarmActive && (now - lastDisplayUpdateMs >= DISPLAY_UPDATE_MS)) {
        lastDisplayUpdateMs = now;
        drawDisplay();
    }
}
