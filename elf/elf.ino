// Elf: motion-triggered light + kicking elf + sound show on an ESP32, with MQTT telemetry and commands.
//
// When the HC-SR501 PIR sensor detects motion, one show runs on a single clock:
//   t = 0                     spotlight (IRLB8721 MOSFET on GPIO26) turns on (or fades up if LIGHT_FADE)
//   t = LEAD_MS - AUDIO_LAG   "play" is published on elf/audio, so the Mac's sound starts at LEAD_MS
//   t = LEAD_MS               relay (GPIO25) closes and holds the elf's button: legs start kicking.
//                             The toy stops on its own after one routine even with the button held,
//                             so every KICK_MS the relay lets go for KICK_GAP_MS and presses again.
//   t = LEAD_MS + SHOW_MS     relay opens, light goes off, "stop" on elf/audio: everything ends together
// then a cooldown. SHOW_MS is the length of the sound (001.mp3, trimmed to the audible part).
//
// Audio on a DFPlayer Mini (audio.h / audio.cpp): at the "play" cue, the show's track (see
// SEQUENCE_TRACKS) starts on the DFPlayer too, and while it plays the show runs until the track
// ends (BUSY pin), instead of for SHOW_MS. Without a DFPlayer, or if the track doesn't start,
// the show falls back to SHOW_MS.
//
// SD card (FAT32, root folder). Copy the files one by one, in this order, onto an empty card:
// the DFPlayer plays "track n" as the n-th file copied, not by file name.
//   0001.mp3  elf intro: the "388 Wentworth Ct" sound (same as 001.mp3 for the Mac, 14.3 s)
// Board: ESP32 Dev Module (30-pin, CP2102 USB). See README.md for the topics.
//
// Fully non-blocking: all timing uses millis(). WiFi and MQTT run in the background
// (ESP-IDF's MQTT client has its own task), so the light works with or without a network.

#include <WiFi.h>
#include "mqtt_client.h"
#include "audio.h"  // DFPlayer Mini wrapper; needs the DFRobotDFPlayerMini library (see README.md)

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
const int LIGHT_PIN = 26;  // -> 220R -> IRLB8721 gate (10k gate-to-GND pulldown); HIGH = 12V spotlight on
const int ELF_PIN   = 25;  // -> PN2222 -> 5V relay across the elf's "try me" button; HIGH = pressed.
                           // (D7 on the UNO version. Never use GPIO6-11 on the ESP32: they run the flash chip.)
const int DFPLAYER_RX_PIN   = 16;  // ESP32 UART2 RX <- DFPlayer TX
const int DFPLAYER_TX_PIN   = 17;  // ESP32 UART2 TX -> 1k -> DFPlayer RX
const int DFPLAYER_BUSY_PIN = 4;   // DFPlayer BUSY: LOW while playing

// ---- Audio (DFPlayer) ----
const uint8_t AUDIO_VOLUME = 30;   // 0..30, default (max: use the speaker's volume knob); change at runtime with elf/config/set {"volume":30}

// Which track plays for each kind of show. The sequence name is the show's reason
// ("motion" = PIR, "manual" = elf/light/set ON, "test" = serial t). Track 0 = no sound for that sequence.
struct SequenceTrack { const char* sequence; uint16_t track; };
const SequenceTrack SEQUENCE_TRACKS[] = {
  { "motion", 1 },  // 0001.mp3, elf intro
  { "manual", 1 },  // 0001.mp3, elf intro
  { "test",   1 },  // 0001.mp3, elf intro
};

#ifndef LED_BUILTIN
#define LED_BUILTIN 2  // blue on-board LED on 30-pin ESP32 DevKit boards
#endif

// ---- Timings (ms) ----
const unsigned long WARMUP_MS    = 60000;  // PIR settle time after power-on
// Defaults below can be changed at runtime with elf/config/set.
const unsigned long LEAD_MS      = 500;    // light turns on this long before the legs and sound start
const bool LIGHT_FADE = false;             // false = spotlight snaps on at t=0 (its built-in driver doesn't dim well with PWM)
const unsigned long SHOW_MS      = 14340;  // legs + sound duration = length of 001.mp3; light off at the end
const unsigned long AUDIO_LAG_MS = 40;     // MQTT + Mac player delay: "play" is sent this much early
const unsigned long COOLDOWN_MS  = 5000;   // ignore motion this long after the show ends

const unsigned long KICK_MS      = 5000;   // during the show, hold the button this long...
const unsigned long KICK_GAP_MS  = 100;    // ...then release it this long and press again (restarts the toy's routine)
const unsigned long ELF_PULSE_MS = 300;
const unsigned long TRACK_START_TIMEOUT_MS = 1000;  // BUSY must go LOW this soon after play, else use SHOW_MS
const unsigned long MAX_TRACK_MS = 120000;          // safety: end the show if BUSY stays LOW this long    // length of the single press fired by the serial "k" test

// Accepted ranges for elf/config/set
const long MAX_LEAD_MS = 5000;                           // 0 .. 5 s
const long MIN_SHOW_MS = 1000, MAX_SHOW_MS = 600000;     // 1 s .. 10 min
const long MAX_COOLDOWN_MS = 600000;                     // 0 .. 10 min
                                                         // audio_lag_ms: 0 .. lead_ms

// ---- MQTT topics ----
#define MQTT_PREFIX "elf"
#define TOPIC(sub) MQTT_PREFIX "/" sub

// LEAD_IN: light fading up, sound cued. SHOW: light full, legs kicking, sound playing.
enum State { WARMUP, IDLE, LEAD_IN, SHOW, COOLDOWN };
const char* const STATE_NAMES[] = { "WARMUP", "IDLE", "LEAD_IN", "SHOW", "COOLDOWN" };

State state = WARMUP;
unsigned long stateStartedAt = 0;  // when the current state began
unsigned long showT0 = 0;          // when the current show began (light starts turning on); all show timing is relative to this
bool audioCued = false;            // "play" already sent for this show
uint16_t showTrack = 0;            // DFPlayer track playing for this show (0 = none)
bool trackTimesShow = false;       // the show ends when showTrack ends (BUSY), not after showMs
const char* showReason = "";       // "motion" or "manual"
int lastPir = LOW;
bool pirNeedsLow = false;          // after a show, PIR must read LOW once before the next motion counts
const unsigned long PREVIEW_MS = 5000;  // length of the serial "p" sound check
unsigned long previewUntil = 0;         // millis() when the preview stops (0 = none running)
bool armed = false;               // PIR warm-up finished
bool autoMode = true;              // false = motion does not start shows (elf/mode/set OFF)
int brightness = 0;                // current PWM level, 0..255
bool elfPressed = false;           // relay currently closed
bool elfTimed = false;             // true = a "k" test press, released after ELF_PULSE_MS
unsigned long elfPressedAt = 0;

unsigned long leadMs     = LEAD_MS;
unsigned long showMs     = SHOW_MS;
unsigned long audioLagMs = AUDIO_LAG_MS;
unsigned long cooldownMs = COOLDOWN_MS;
uint8_t audioVolume      = AUDIO_VOLUME;

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
// Returns false if it could not be queued.
bool publish(const char* topic, const char* payload, bool retain, int qos = 1) {
  if (mqtt == nullptr || !mqttConnected) return false;
  return esp_mqtt_client_enqueue(mqtt, topic, payload, 0, qos, retain, true) >= 0;
}

// Sound cues are timing-critical, so they're sent right away from this task instead of being queued
// for the MQTT task (which only sends queued messages on its next pass, a few hundred ms later).
// QoS 0: a "play" redelivered late after a reconnect would restart the sound mid-show.
void cueAudio(const char* command) {
  printTimestamp();
  if (mqtt != nullptr && mqttConnected &&
      esp_mqtt_client_publish(mqtt, TOPIC("audio"), command, 0, 0, 0) >= 0) {
    Serial.printf("audio -> %s\n", command);
  } else {
    Serial.printf("audio -> %s NOT SENT (MQTT offline)\n", command);
  }
}

void publishConfig() {
  char json[160];
  snprintf(json, sizeof(json), "{\"lead_ms\":%lu,\"show_ms\":%lu,\"audio_lag_ms\":%lu,\"cooldown_ms\":%lu,\"volume\":%u}",
           leadMs, showMs, audioLagMs, cooldownMs, audioVolume);
  publish(TOPIC("config"), json, true);
}

void publishInfo() {
  char json[128];
  snprintf(json, sizeof(json), "{\"ip\":\"%s\",\"rssi\":%d,\"client_id\":\"%s\",\"uptime_s\":%lu}",
           WiFi.localIP().toString().c_str(), WiFi.RSSI(), mqttClientId, millis() / 1000);
  publish(TOPIC("info"), json, true);
}

bool showRunning() {
  return state == LEAD_IN || state == SHOW;
}

// Publishes every retained topic so new subscribers (and the broker after a restart) see current values.
void publishSnapshot() {
  publish(TOPIC("status"), "online", true);
  publish(TOPIC("state"), STATE_NAMES[state], true);
  publish(TOPIC("light"), showRunning() ? "ON" : "OFF", true);
  publish(TOPIC("motion"), lastPir == HIGH ? "1" : "0", true);
  publish(TOPIC("mode"), autoMode ? "AUTO" : "OFF", true);
  publishConfig();
  publishInfo();
}

// ---------------------------------------------------------------- state machine

void releaseElf();

void enterState(State next) {
  // The elf is held only during the show; any other state lets go of it.
  if (next != SHOW) releaseElf();
  state = next;
  stateStartedAt = millis();
  printTimestamp();
  Serial.printf("state -> %s\n", STATE_NAMES[next]);
  publish(TOPIC("state"), STATE_NAMES[next], true);
}

// Full on and off drive the pin as plain GPIO: analogWrite(255) is still ~99.6% PWM, which the
// spotlight's built-in AC/DC driver doesn't like (it runs dimmer). PWM is used only for in-between levels.
bool lightPwm = true;  // true forces the first call to set the pin up as plain GPIO
void setBrightness(int level) {
  level = constrain(level, 0, 255);
  if (level == 0 || level == 255) {
    if (lightPwm || level != brightness) {
      pinMode(LIGHT_PIN, OUTPUT);  // detaches the PWM channel
      digitalWrite(LIGHT_PIN, level ? HIGH : LOW);
      lightPwm = false;
    }
  } else {
    analogWrite(LIGHT_PIN, level);
    lightPwm = true;
  }
  brightness = level;
}

void pressElf(const char* reason, bool timed);

uint16_t trackForSequence(const char* sequence) {
  for (const SequenceTrack& entry : SEQUENCE_TRACKS) {
    if (strcmp(entry.sequence, sequence) == 0) return entry.track;
  }
  return 0;
}

// At the sound cue: the Mac gets "play" over MQTT, and the DFPlayer (if present) starts the sequence's track.
void startShowAudio() {
  cueAudio("play");
  uint16_t track = trackForSequence(showReason);
  showTrack = (track != 0 && audio::playTrack(track)) ? track : 0;
  trackTimesShow = showTrack != 0;
  if (track != 0 && showTrack == 0) logLine("no DFPlayer: show timed by show_ms");
}

// t = 0: the light starts turning on. Everything else in the show is timed from here.
void startShow(const char* reason) {
  pirNeedsLow = true;
  showT0 = millis();
  audioCued = false;
  showReason = reason;
  printTimestamp();
  Serial.printf("SHOW START (%s)\n", reason);
  enterState(LEAD_IN);
  publish(TOPIC("light"), "ON", true);
}

// Ends the show: legs, light and sound stop together. `aborted` = cut short by a command.
void endShow(bool aborted) {
  setBrightness(0);
  releaseElf();
  cueAudio("stop");
  // Stop the track unless it already ended by itself (the normal end of a track-timed show).
  if (showTrack != 0 && (aborted || !trackTimesShow || audio::isPlaying())) audio::stop();
  showTrack = 0;
  trackTimesShow = false;
  logLine(aborted ? "SHOW ABORTED, LIGHT OFF" : "SHOW END, LIGHT OFF");
  publish(TOPIC("light"), "OFF", true);
  enterState(COOLDOWN);
}

// ---------------------------------------------------------------- elf relay

// Closes the relay. timed = true releases it after ELF_PULSE_MS (the "k" test);
// timed = false holds it until releaseElf() (the light show).
void pressElf(const char* reason, bool timed) {
  if (elfPressed) {
    if (!timed && elfTimed) {
      elfTimed = false;  // a test press is running: keep holding it for the show instead
      logLine("elf -> HELD");
    }
    return;
  }
  digitalWrite(ELF_PIN, HIGH);
  elfPressed = true;
  elfTimed = timed;
  elfPressedAt = millis();
  printTimestamp();
  Serial.printf("ELF KICK (%s)\n", reason);
  logLine(timed ? "elf -> PRESSED" : "elf -> HELD");
  publish(TOPIC("kick"), reason, false);
}

// During the show: hold the button for KICK_MS, let go for KICK_GAP_MS, press again, and so on.
// The cycle is timed from the moment the legs started (showT0 + leadMs), so it never drifts.
void updateKickCycle(unsigned long sinceKickStart) {
  unsigned long cycle = sinceKickStart / (KICK_MS + KICK_GAP_MS);
  bool hold = sinceKickStart % (KICK_MS + KICK_GAP_MS) < KICK_MS;
  if (hold && !elfPressed) {
    digitalWrite(ELF_PIN, HIGH);
    elfPressed = true;
    elfTimed = false;
    elfPressedAt = millis();
    printTimestamp();
    Serial.printf("elf -> HELD again (re-press %lu)\n", cycle);
  } else if (!hold && elfPressed) {
    digitalWrite(ELF_PIN, LOW);
    elfPressed = false;
    logLine("elf -> gap");
  }
}

void releaseElf() {
  if (!elfPressed) return;
  digitalWrite(ELF_PIN, LOW);
  elfPressed = false;
  elfTimed = false;
  logLine("elf -> RELEASED");
}

// Ends a timed test press once ELF_PULSE_MS is up. Called every loop.
void updateElf() {
  if (elfPressed && elfTimed && millis() - elfPressedAt >= ELF_PULSE_MS) {
    releaseElf();
  }
}

void handleModeCommand(const char* payload);

// Serial test commands (type the letter, then Enter):
//   k  fires one short elf press without the PIR
//   r  turns the motion sensor on/off: same as elf/mode/set AUTO / OFF (a running show isn't stopped)
//   p  plays the first 5 s of the track on the DFPlayer (sound check, no elf or light)
//   a  prints whether the DFPlayer was found
//   t  starts a full show now, without the PIR (sequence "test"), like elf/light/set ON
void handleSerialInput() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c == 'k' || c == 'K') {
      if (!armed) {
        logLine("k ignored: the elf stays off during PIR warm-up");
      } else {
        pressElf("serial test", true);
      }
    } else if (c == 'r' || c == 'R') {
      handleModeCommand(autoMode ? "OFF" : "AUTO");
      logLine(autoMode ? "motion sensor ON: motion starts shows" : "motion sensor OFF: motion is ignored");
    } else if (c == 'p' || c == 'P') {
      if (showRunning()) {
        logLine("p ignored: a show is running");
      } else if (!audio::ready()) {
        logLine("p ignored: no DFPlayer");
      } else if (audio::playTrack(trackForSequence("test"))) {
        previewUntil = millis() + PREVIEW_MS;
        logLine("preview: playing the first 5 s of the track");
      }
    } else if (c == 'a' || c == 'A') {
      logLine(audio::statusText());
    } else if (c == 't' || c == 'T') {
      if (showRunning()) {
        logLine("t ignored: a show is already running");
      } else {
        startShow("test");
      }
    }
  }
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

// ON starts a show now (even in mode OFF or during warm-up); OFF cuts a running show short.
void handleLightCommand(const char* payload) {
  if (strcasecmp(payload, "ON") == 0) {
    if (showRunning()) {
      logLine("ignored: a show is already running");
    } else {
      startShow("manual");
    }
  } else if (strcasecmp(payload, "OFF") == 0) {
    if (showRunning()) endShow(true);
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

// Changes take effect from the next show. They're rejected while a show is running, so a show
// in progress can never fall out of sync with its sound.
void handleConfigCommand(const char* payload) {
  long lead = leadMs, show = showMs, lag = audioLagMs, cool = cooldownMs, vol = audioVolume;
  int foundLead = jsonInt(payload, "lead_ms", lead);
  int foundShow = jsonInt(payload, "show_ms", show);
  int foundLag  = jsonInt(payload, "audio_lag_ms", lag);
  int foundCool = jsonInt(payload, "cooldown_ms", cool);
  int foundVol  = jsonInt(payload, "volume", vol);
  bool timing = foundLead + foundShow + foundLag + foundCool > 0;

  if (payload[0] != '{' || foundLead < 0 || foundShow < 0 || foundLag < 0 || foundCool < 0 ||
      foundVol < 0 || (!timing && foundVol == 0)) {
    logLine("ignored: config/set expects JSON like {\"lead_ms\":500,\"show_ms\":14340,\"audio_lag_ms\":40,\"cooldown_ms\":5000,\"volume\":25}");
    return;
  }
  if (lead > MAX_LEAD_MS || show < MIN_SHOW_MS || show > MAX_SHOW_MS ||
      lag > lead || cool > MAX_COOLDOWN_MS || vol > 30) {
    logLine("ignored: config out of range (lead_ms 0-5000, show_ms 1000-600000, audio_lag_ms 0-lead_ms, cooldown_ms 0-600000, volume 0-30)");
    return;
  }

  // Volume can change any time, even during a show: it doesn't affect the timing.
  if (foundVol == 1 && vol != audioVolume) {
    audioVolume = vol;
    audio::setVolume(audioVolume);
    printTimestamp();
    Serial.printf("config -> volume=%u\n", audioVolume);
  }
  if (timing) {
    if (showRunning()) {
      logLine("ignored: timings can't change during a show (it would break the sync); send them again after");
    } else {
      leadMs = lead;
      showMs = show;
      audioLagMs = lag;
      cooldownMs = cool;
      printTimestamp();
      Serial.printf("config -> lead_ms=%lu show_ms=%lu audio_lag_ms=%lu cooldown_ms=%lu\n",
                    leadMs, showMs, audioLagMs, cooldownMs);
    }
  }
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
    if (showRunning()) cueAudio("stop");
    audio::stop();
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
  pinMode(PIR_PIN, INPUT_PULLDOWN);  // an unconnected or broken-wire PIR reads LOW instead of floating and toggling
  pinMode(LIGHT_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  setBrightness(0);

  Serial.begin(115200);
  logLine("Elf starting, PIR warming up for 60 s");

  mqttInbox = xQueueCreate(10, sizeof(MqttMessage));

  audio::begin(DFPLAYER_RX_PIN, DFPLAYER_TX_PIN, DFPLAYER_BUSY_PIN, AUDIO_VOLUME);  // finds the player in the background

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep delays sending by up to a few hundred ms, which breaks the sound sync
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  printTimestamp();
  Serial.printf("WiFi connecting to %s\n", WIFI_SSID);

  enterState(WARMUP);
}

void loop() {
  updateElf();
  audio::update();
  checkNetwork();
  handleSerialInput();

  // End the serial "p" preview (also if a show started meanwhile: the show owns the player then).
  if (previewUntil != 0 && (showRunning() || (long)(millis() - previewUntil) >= 0)) {
    if (!showRunning()) audio::stop();
    previewUntil = 0;
  }

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
        if (!armed) {
          logLine("ARMED");
          logLine(audio::statusText());  // repeated here: the boot-time line is easy to miss
        }
        armed = true;
        enterState(IDLE);
      }
      break;

    case IDLE:
      if (pir == LOW) pirNeedsLow = false;
      if (autoMode && pir == HIGH && !pirNeedsLow) {
        logLine("MOTION");
        startShow("motion");
      }
      break;

    case LEAD_IN: {
      // All show timing is measured from showT0, not from state changes, so loop jitter never adds up.
      unsigned long t = now - showT0;
      if (!audioCued && t >= leadMs - audioLagMs) {
        audioCued = true;
        startShowAudio();  // sound comes out audioLagMs from now = when the legs start
      }
      if (t >= leadMs) {
        setBrightness(255);
        enterState(SHOW);
        pressElf(showReason, false);  // legs start: held (with a short re-press every KICK_MS) until the show ends
      } else {
        setBrightness(LIGHT_FADE ? (int)(255UL * t / leadMs) : 255);  // leadMs > 0 here, since t < leadMs
      }
      break;
    }

    case SHOW: {
      // Legs kicking + sound playing. Motion is ignored until the cooldown is over.
      unsigned long t = now - showT0;
      if (trackTimesShow) {
        // DFPlayer track: the show lasts as long as the track plays (BUSY LOW).
        if (audio::trackStarted()) {
          if (!audio::isPlaying()) {
            logLine("track finished");
            endShow(false);
            break;
          }
          if (t >= leadMs + MAX_TRACK_MS) {
            logLine("track still playing after MAX_TRACK_MS (BUSY stuck?): ending show");
            endShow(false);
            break;
          }
        } else if (audio::msSincePlay() >= TRACK_START_TIMEOUT_MS) {
          logLine("track didn't start (BUSY never went LOW): show timed by show_ms");
          trackTimesShow = false;
        }
      }
      if (!trackTimesShow && t >= leadMs + showMs) {
        endShow(false);
        break;
      }
      updateKickCycle(t - leadMs);
      break;
    }

    case COOLDOWN:
      if (elapsed >= cooldownMs) {
        enterState(armed ? IDLE : WARMUP);
      }
      break;
  }
}
