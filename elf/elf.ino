// Elf: motion-activated light on an ESP32, with MQTT telemetry and remote commands.
//
// When the HC-SR501 PIR sensor detects motion, a light (driven by a PN2222 transistor
// on GPIO26) fades on, holds, fades out, then cools down. Every change is published
// over MQTT, and the light, mode and timings can be controlled over MQTT.
// Board: ESP32 Dev Module (30-pin, CP2102 USB). See README.md for the topics.
//
// Fully non-blocking: all timing uses millis(). WiFi and MQTT run in the background
// (ESP-IDF's MQTT client has its own task), so the light works with or without a network.

#include <WiFi.h>
#include "mqtt_client.h"

#if __has_include("secrets.h")
#include "secrets.h"  // WiFi and MQTT credentials (not committed)
#else
#error "Missing secrets.h: copy secrets.example.h to secrets.h and fill in your details."
#endif

#if !defined(MQTT_HOST) || !defined(MQTT_PORT) || !defined(MQTT_USER) || !defined(MQTT_PASSWORD)
#error "secrets.h is missing the MQTT settings: copy them from secrets.example.h."
#endif

// ---- Pins ----
const int PIR_PIN   = 27;  // HC-SR501 OUT (jumper on H, time knob at minimum)
const int LIGHT_PIN = 26;  // PWM -> 1k -> PN2222 base; HIGH = LEDs on
const int ELF_PIN   = 25;  // -> PN2222 -> 5V relay across the elf's "try me" button; HIGH = pressed.
                           // (D7 on the UNO version. Never use GPIO6-11 on the ESP32: they run the flash chip.)

#ifndef LED_BUILTIN
#define LED_BUILTIN 2  // blue on-board LED on 30-pin ESP32 DevKit boards
#endif

// ---- Timings (ms) ----
const unsigned long WARMUP_MS   = 60000;  // PIR settle time after power-on
// Defaults below can be changed at runtime with elf/config/set.
const unsigned long LIGHT_ON_MS = 10000;  // light stays on this long from the moment motion is detected
const unsigned long FADE_MS     = 1000;   // fade-in and fade-out duration
const unsigned long COOLDOWN_MS = 5000;   // ignore motion this long after the light turns off

// Elf animatronic: one button "press" per motion show, after the light has fully faded in.
const unsigned long ELF_PULSE_MS = 300;   // how long the relay is closed (one press = one kick cycle)
const unsigned long ELF_DELAY_MS = 0;     // wait this long after the fade-in finishes before pressing

// Accepted ranges for elf/config/set
const long MIN_ON_MS = 1000,  MAX_ON_MS = 3600000;      // 1 s .. 1 h
const long MAX_FADE_MS = 10000;                         // 0 .. 10 s
const long MAX_COOLDOWN_MS = 600000;                    // 0 .. 10 min

// ---- MQTT topics ----
#define MQTT_PREFIX "elf"
#define TOPIC(sub) MQTT_PREFIX "/" sub

enum State { WARMUP, IDLE, FADING_IN, ON, FADING_OUT, COOLDOWN };
const char* const STATE_NAMES[] = { "WARMUP", "IDLE", "FADING_IN", "ON", "FADING_OUT", "COOLDOWN" };

State state = WARMUP;
unsigned long stateStartedAt = 0;  // when the current state began
unsigned long motionAt = 0;        // when the motion that turned the light on was detected
int lastPir = LOW;
bool armed = false;                // PIR warm-up finished
bool autoMode = true;              // false = motion does not trigger the light (elf/mode/set OFF)
bool manualOn = false;             // light turned on by elf/light/set ON; stays on until OFF
int brightness = 0;                // current PWM level, 0..255
int fadeFrom = 0;                  // brightness when the current fade started
bool kickPending = false;          // this motion show still owes the elf its one press
bool elfPulsing = false;           // relay currently closed
unsigned long elfPulseStartedAt = 0;

unsigned long lightOnMs  = LIGHT_ON_MS;
unsigned long fadeMs     = FADE_MS;
unsigned long cooldownMs = COOLDOWN_MS;

// MQTT client and the hand-off between its background task and loop().
struct MqttMessage {
  char topic[64];
  char payload[160];
  bool retained;
  bool tooLong;
};
esp_mqtt_client_handle_t mqtt = nullptr;
QueueHandle_t mqttInbox = nullptr;
volatile bool mqttConnected = false;
volatile bool mqttJustConnected = false;
char mqttClientId[24];

// ---------------------------------------------------------------- logging

void printTimestamp() {
  unsigned long now = millis();
  Serial.printf("[%lu.%03lus] ", now / 1000, now % 1000);
}

void logLine(const char* message) {
  printTimestamp();
  Serial.println(message);
}

// ---------------------------------------------------------------- MQTT out

// Queues a message for the MQTT task to send; never blocks. Dropped when offline,
// which is fine because retained topics are republished on every connect.
void publish(const char* topic, const char* payload, bool retain) {
  if (mqtt == nullptr || !mqttConnected) return;
  esp_mqtt_client_enqueue(mqtt, topic, payload, 0, 1, retain, true);
}

void publishConfig() {
  char json[96];
  snprintf(json, sizeof(json), "{\"on_ms\":%lu,\"fade_ms\":%lu,\"cooldown_ms\":%lu}",
           lightOnMs, fadeMs, cooldownMs);
  publish(TOPIC("config"), json, true);
}

void publishInfo() {
  char json[128];
  snprintf(json, sizeof(json), "{\"ip\":\"%s\",\"rssi\":%d,\"client_id\":\"%s\",\"uptime_s\":%lu}",
           WiFi.localIP().toString().c_str(), WiFi.RSSI(), mqttClientId, millis() / 1000);
  publish(TOPIC("info"), json, true);
}

bool lightIsOn() {
  return state == FADING_IN || state == ON || state == FADING_OUT;
}

// Publishes every retained topic so new subscribers (and the broker after a restart) see current values.
void publishSnapshot() {
  publish(TOPIC("status"), "online", true);
  publish(TOPIC("state"), STATE_NAMES[state], true);
  publish(TOPIC("light"), lightIsOn() ? "ON" : "OFF", true);
  publish(TOPIC("motion"), lastPir == HIGH ? "1" : "0", true);
  publish(TOPIC("mode"), autoMode ? "AUTO" : "OFF", true);
  publishConfig();
  publishInfo();
}

// ---------------------------------------------------------------- state machine

void enterState(State next) {
  state = next;
  stateStartedAt = millis();
  printTimestamp();
  Serial.printf("state -> %s\n", STATE_NAMES[next]);
  publish(TOPIC("state"), STATE_NAMES[next], true);
}

void setBrightness(int level) {
  brightness = level;
  analogWrite(LIGHT_PIN, level);
}

// Fades from the current brightness, so a fade can reverse midway.
void startFadeIn(bool manual) {
  manualOn = manual;
  kickPending = !manual;  // only motion-triggered shows press the elf
  fadeFrom = brightness;
  enterState(FADING_IN);
  publish(TOPIC("light"), "ON", true);
}

void startFadeOut() {
  manualOn = false;
  kickPending = false;  // show over: a press that hasn't happened yet is cancelled
  fadeFrom = brightness;
  enterState(FADING_OUT);
}

// ---------------------------------------------------------------- elf relay

// Closes the relay for ELF_PULSE_MS. A pulse already in progress is never extended.
void startElfPulse(const char* reason) {
  if (elfPulsing) {
    logLine("elf: pulse already running, ignored");
    return;
  }
  digitalWrite(ELF_PIN, HIGH);
  elfPulsing = true;
  elfPulseStartedAt = millis();
  printTimestamp();
  Serial.printf("ELF KICK (%s)\n", reason);
  logLine("elf -> PRESSED");
  publish(TOPIC("kick"), reason, false);
}

// Releases the relay once the pulse time is up. Called every loop.
void updateElf() {
  if (elfPulsing && millis() - elfPulseStartedAt >= ELF_PULSE_MS) {
    digitalWrite(ELF_PIN, LOW);
    elfPulsing = false;
    logLine("elf -> RELEASED");
  }
}

// Serial test command: "k" fires one elf pulse without the PIR.
void handleSerialInput() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c == 'k' || c == 'K') {
      if (!armed) {
        logLine("k ignored: the elf stays off during PIR warm-up");
      } else {
        startElfPulse("serial test");
      }
    }
  }
}

// Level for a fade that is `elapsed` ms into a `fadeMs` fade from `from` to `to`.
int fadeLevel(int from, int to, unsigned long elapsed) {
  return from + (int)((long)(to - from) * (long)elapsed / (long)fadeMs);
}

// ---------------------------------------------------------------- MQTT in

// Runs on the MQTT task: only copies messages into the queue; loop() acts on them.
void onMqttEvent(void*, esp_event_base_t, int32_t eventId, void* eventData) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;
  switch ((esp_mqtt_event_id_t)eventId) {
    case MQTT_EVENT_CONNECTED:
      mqttConnected = true;
      mqttJustConnected = true;
      break;
    case MQTT_EVENT_DISCONNECTED:
      mqttConnected = false;
      break;
    case MQTT_EVENT_DATA: {
      if (event->current_data_offset != 0) return;  // continuation of a fragmented message
      MqttMessage msg = {};
      if (event->topic_len >= (int)sizeof(msg.topic)) return;
      memcpy(msg.topic, event->topic, event->topic_len);
      msg.tooLong = event->total_data_len >= (int)sizeof(msg.payload);
      if (!msg.tooLong) memcpy(msg.payload, event->data, event->data_len);
      msg.retained = event->retain;
      xQueueSend(mqttInbox, &msg, 0);  // drop if the queue is full
      break;
    }
    default:
      break;
  }
}

void startMqtt() {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(mqttClientId, sizeof(mqttClientId), "elf-%04X%08lX", (unsigned)(mac >> 32) & 0xFFFF, (unsigned long)(mac & 0xFFFFFFFF));

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.hostname = MQTT_HOST;
  cfg.broker.address.port = MQTT_PORT;
  cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  cfg.credentials.client_id = mqttClientId;
  if (strlen(MQTT_USER) > 0) {
    cfg.credentials.username = MQTT_USER;
    cfg.credentials.authentication.password = MQTT_PASSWORD;
  }
  // If elf drops off without saying goodbye, the broker publishes this for it.
  cfg.session.last_will.topic = TOPIC("status");
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.session.keepalive = 15;

  mqtt = esp_mqtt_client_init(&cfg);
  esp_mqtt_client_register_event(mqtt, MQTT_EVENT_ANY, onMqttEvent, nullptr);
  esp_mqtt_client_start(mqtt);
  printTimestamp();
  Serial.printf("MQTT connecting to %s:%d as %s\n", MQTT_HOST, MQTT_PORT, mqttClientId);
}

// Finds "key": <non-negative integer> in a flat JSON object.
// Returns 1 if found and valid, 0 if the key is absent, -1 if its value is not a non-negative integer.
int jsonInt(const char* json, const char* key, long& out) {
  char pattern[24];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char* p = strstr(json, pattern);
  if (p == nullptr) return 0;
  p += strlen(pattern);
  while (*p == ' ' || *p == '\t') p++;
  if (*p != ':') return -1;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (*p < '0' || *p > '9') return -1;
  char* end;
  long value = strtol(p, &end, 10);
  if (strchr(",} \t\r\n", *end) == nullptr) return -1;
  out = value;
  return 1;
}

void handleLightCommand(const char* payload) {
  if (strcasecmp(payload, "ON") == 0) {
    if (state == FADING_IN || state == ON) {
      manualOn = true;  // already on: just keep it on until OFF
      logLine("light held on until OFF");
    } else {
      startFadeIn(true);
    }
  } else if (strcasecmp(payload, "OFF") == 0) {
    if (state == FADING_IN || state == ON) startFadeOut();
  } else {
    logLine("ignored: light/set expects ON or OFF");
  }
}

void handleModeCommand(const char* payload) {
  if (strcasecmp(payload, "AUTO") == 0) {
    autoMode = true;
  } else if (strcasecmp(payload, "OFF") == 0) {
    autoMode = false;
  } else {
    logLine("ignored: mode/set expects AUTO or OFF");
    return;
  }
  printTimestamp();
  Serial.printf("mode -> %s\n", autoMode ? "AUTO" : "OFF");
  publish(TOPIC("mode"), autoMode ? "AUTO" : "OFF", true);
}

void handleConfigCommand(const char* payload) {
  long onMs = lightOnMs, newFadeMs = fadeMs, coolMs = cooldownMs;
  int foundOn = jsonInt(payload, "on_ms", onMs);
  int foundFade = jsonInt(payload, "fade_ms", newFadeMs);
  int foundCool = jsonInt(payload, "cooldown_ms", coolMs);

  if (payload[0] != '{' || foundOn < 0 || foundFade < 0 || foundCool < 0 ||
      foundOn + foundFade + foundCool == 0) {
    logLine("ignored: config/set expects JSON like {\"on_ms\":10000,\"fade_ms\":1000,\"cooldown_ms\":5000}");
    return;
  }
  if (onMs < MIN_ON_MS || onMs > MAX_ON_MS || newFadeMs > MAX_FADE_MS ||
      coolMs > MAX_COOLDOWN_MS || newFadeMs > onMs) {
    logLine("ignored: config out of range (on_ms 1000-3600000, fade_ms 0-10000 and <= on_ms, cooldown_ms 0-600000)");
    return;
  }
  lightOnMs = onMs;
  fadeMs = newFadeMs;
  cooldownMs = coolMs;
  printTimestamp();
  Serial.printf("config -> on_ms=%lu fade_ms=%lu cooldown_ms=%lu\n", lightOnMs, fadeMs, cooldownMs);
  publishConfig();
}

void handleCmd(const char* payload) {
  if (strcasecmp(payload, "PING") == 0) {
    char json[96];
    snprintf(json, sizeof(json), "{\"uptime_s\":%lu,\"rssi\":%d,\"state\":\"%s\"}",
             millis() / 1000, WiFi.RSSI(), STATE_NAMES[state]);
    publish(TOPIC("pong"), json, false);
  } else if (strcasecmp(payload, "RESTART") == 0) {
    logLine("restarting");
    digitalWrite(ELF_PIN, LOW);
    setBrightness(0);
    if (mqtt != nullptr && mqttConnected) {
      esp_mqtt_client_publish(mqtt, TOPIC("status"), "offline", 0, 1, 1);
      delay(500);  // let the MQTT task finish sending before the reboot
    }
    ESP.restart();
  } else {
    logLine("ignored: cmd expects PING or RESTART");
  }
}

void handleMqttMessage(const MqttMessage& msg) {
  printTimestamp();
  Serial.printf("MQTT in: %s %s%s\n", msg.topic, msg.tooLong ? "(payload too long)" : msg.payload,
                msg.retained ? " (retained)" : "");
  if (msg.tooLong) return;

  if (strcmp(msg.topic, TOPIC("mode/set")) == 0) {
    handleModeCommand(msg.payload);
  } else if (strcmp(msg.topic, TOPIC("config/set")) == 0) {
    handleConfigCommand(msg.payload);
  } else if (msg.retained) {
    // light/set and cmd are one-shot: a retained copy would replay on every reconnect (a RESTART loop).
    logLine("ignored: retained one-shot command (publish without -r)");
  } else if (strcmp(msg.topic, TOPIC("light/set")) == 0) {
    handleLightCommand(msg.payload);
  } else if (strcmp(msg.topic, TOPIC("cmd")) == 0) {
    handleCmd(msg.payload);
  }
}

// ---------------------------------------------------------------- network

void checkNetwork() {
  static bool wifiWasConnected = false;
  bool wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected != wifiWasConnected) {
    wifiWasConnected = wifiConnected;
    printTimestamp();
    if (wifiConnected) {
      Serial.printf("WiFi connected to %s, IP %s, signal %d dBm\n", WIFI_SSID,
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (mqtt == nullptr) startMqtt();  // the client reconnects on its own after this
    } else {
      Serial.println("WiFi disconnected, retrying");
    }
  }

  static bool mqttWasConnected = false;
  if (mqttConnected != mqttWasConnected) {
    mqttWasConnected = mqttConnected;
    logLine(mqttConnected ? "MQTT connected" : "MQTT disconnected, retrying");
  }

  if (mqttJustConnected) {
    mqttJustConnected = false;
    esp_mqtt_client_subscribe_single(mqtt, TOPIC("light/set"), 1);
    esp_mqtt_client_subscribe_single(mqtt, TOPIC("mode/set"), 1);
    esp_mqtt_client_subscribe_single(mqtt, TOPIC("config/set"), 1);
    esp_mqtt_client_subscribe_single(mqtt, TOPIC("cmd"), 1);
    publishSnapshot();
  }

  MqttMessage msg;
  while (mqttInbox != nullptr && xQueueReceive(mqttInbox, &msg, 0) == pdTRUE) {
    handleMqttMessage(msg);
  }
}

// ---------------------------------------------------------------- main

void setup() {
  pinMode(ELF_PIN, OUTPUT);
  digitalWrite(ELF_PIN, LOW);  // first thing: relay open at boot and through warm-up
  pinMode(PIR_PIN, INPUT);
  pinMode(LIGHT_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  setBrightness(0);

  Serial.begin(115200);
  logLine("Elf starting, PIR warming up for 60 s");

  mqttInbox = xQueueCreate(10, sizeof(MqttMessage));

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  printTimestamp();
  Serial.printf("WiFi connecting to %s\n", WIFI_SSID);

  enterState(WARMUP);
}

void loop() {
  updateElf();
  checkNetwork();
  handleSerialInput();

  unsigned long now = millis();
  unsigned long elapsed = now - stateStartedAt;

  // Sensor debug: the built-in LED mirrors the raw PIR output, and edges are logged and published.
  int pir = digitalRead(PIR_PIN);
  digitalWrite(LED_BUILTIN, pir);
  if (pir != lastPir) {
    logLine(pir == HIGH ? "PIR: HIGH" : "PIR: LOW");
    publish(TOPIC("motion"), pir == HIGH ? "1" : "0", true);
    lastPir = pir;
  }

  switch (state) {
    case WARMUP:
      if (now >= WARMUP_MS) {
        if (!armed) logLine("ARMED");
        armed = true;
        enterState(IDLE);
      }
      break;

    case IDLE:
      if (autoMode && pir == HIGH) {
        motionAt = now;
        logLine("MOTION");
        startFadeIn(false);
      }
      break;

    case FADING_IN:
      if (elapsed >= fadeMs) {
        setBrightness(255);
        enterState(ON);
      } else {
        setBrightness(fadeLevel(fadeFrom, 255, elapsed));
      }
      break;

    case ON:
      // Motion-triggered: the timer runs from the original motion; further motion does not extend it.
      // Manually turned on: stays on until elf/light/set OFF.
      if (kickPending && elapsed >= ELF_DELAY_MS) {
        kickPending = false;
        startElfPulse("motion");
      }
      if (!manualOn && now - motionAt >= lightOnMs) {
        startFadeOut();
      }
      break;

    case FADING_OUT:
      if (elapsed >= fadeMs) {
        setBrightness(0);
        logLine("LIGHT OFF");
        publish(TOPIC("light"), "OFF", true);
        enterState(COOLDOWN);
      } else {
        setBrightness(fadeLevel(fadeFrom, 0, elapsed));
      }
      break;

    case COOLDOWN:
      if (elapsed >= cooldownMs) {
        enterState(armed ? IDLE : WARMUP);
      }
      break;
  }
}
