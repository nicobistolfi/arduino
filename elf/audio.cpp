#include "audio.h"

#include <atomic>
#include <DFRobotDFPlayerMini.h>

namespace audio {
namespace {

const unsigned long BOOT_WAIT_MS     = 1500;  // DFPlayer needs ~1-2 s after power-up before it answers
const unsigned long RETRY_MS         = 10000; // if not found, look for it again this often (e.g. after a wiring fix)
const unsigned long SETTLE_MS        = 100;   // after playTrack(), BUSY isn't trusted for this long
const unsigned long BUSY_DEBOUNCE_MS = 20;    // BUSY must hold a level this long (it flickers at track start)

enum Status : uint8_t { PENDING, READY, MISSING };

HardwareSerial dfSerial(2);
DFRobotDFPlayerMini player;
std::atomic<uint8_t> status{PENDING};  // written by the init task, read by loop()
int busy = -1;
std::atomic<uint8_t> wantedVolume{25};  // set by begin()/setVolume(); update() sends it when it changes
uint8_t sentVolume = 255;               // last volume sent to the player (255 = none yet)

int busyStable = HIGH;          // debounced BUSY level (HIGH = idle)
int busyCandidate = HIGH;
unsigned long busyCandidateSince = 0;
unsigned long playAt = 0;
bool playRequested = false;
bool started = false;

void logf(const char* format, ...) {
  unsigned long now = millis();
  Serial.printf("[%lu.%03lus] audio: ", now / 1000, now % 1000);
  va_list args;
  va_start(args, format);
  Serial.vprintf(format, args);
  va_end(args);
  Serial.println();
}

// The library's begin() reports success without checking for a player in ACK mode, and always
// in no-ACK mode, so presence is confirmed with a query (readVolume() returns -1 on a 500 ms timeout).
bool tryInit(bool ack, bool reset) {
  player.begin(dfSerial, ack, reset);
  uint8_t type = player.readType();  // after a reset: the "card online" message, or a timeout error
  bool online = reset && (type == DFPlayerCardOnline || type == DFPlayerUSBOnline || type == DFPlayerCardUSBOnline);
  return player.readVolume() >= 0 || online;
}

// First run (arg = nullptr): at boot, logs every step. Retries (arg != nullptr): log only on success,
// so a missing player doesn't fill the log every 10 s.
void initTask(void* arg) {
  bool retry = arg != nullptr;
  if (!retry) vTaskDelay(pdMS_TO_TICKS(BOOT_WAIT_MS));
  bool found = tryInit(true, false);
  if (found) {
    logf("DFPlayer found (ACK mode)");
  } else {
    if (!retry) logf("no answer in ACK mode, retrying without ACK and with a reset (clone boards)");
    found = tryInit(false, true);
    if (found) logf("DFPlayer found (no-ACK mode, after reset)");
  }
  if (found) {
    // From here on, commands never wait for ACKs: a lost ACK would stall the next command up to 500 ms.
    // (disableACK() is private; begin() without a reset just switches the mode, sending nothing.)
    player.begin(dfSerial, false, false);
    sentVolume = wantedVolume;
    player.volume(sentVolume);
    logf("ready, volume %u", sentVolume);
    status = READY;
  } else {
    if (!retry) logf("DFPlayer NOT found: check wiring and SD card. Running without audio, looking again every 10 s.");
    status = MISSING;
  }
  vTaskDelete(nullptr);
}

}  // namespace

void begin(int rxPin, int txPin, int busyPin, uint8_t volume) {
  busy = busyPin;
  wantedVolume = volume > 30 ? 30 : volume;
  pinMode(busy, INPUT_PULLUP);  // reads HIGH (idle) if nothing is connected
  dfSerial.begin(9600, SERIAL_8N1, rxPin, txPin);
  xTaskCreate(initTask, "dfplayer-init", 4096, nullptr, 1, nullptr);
}

bool ready() {
  return status == READY;
}

void update() {
  unsigned long now = millis();
  if (status == MISSING) {
    // Look for the player again every RETRY_MS, in the background like at boot.
    static unsigned long missingSince = 0;
    if (missingSince == 0) missingSince = now | 1;
    if (now - missingSince >= RETRY_MS) {
      missingSince = 0;
      status = PENDING;
      xTaskCreate(initTask, "dfplayer-init", 4096, (void*)1, 1, nullptr);
    }
    return;
  }
  if (!ready()) return;

  if (wantedVolume != sentVolume) {  // a setVolume() made before or during init, or since
    sentVolume = wantedVolume;
    player.volume(sentVolume);
    logf("volume %u", sentVolume);
  }

  int raw = digitalRead(busy);
  if (raw != busyCandidate) {
    busyCandidate = raw;
    busyCandidateSince = now;
  } else if (busyStable != busyCandidate && now - busyCandidateSince >= BUSY_DEBOUNCE_MS) {
    busyStable = busyCandidate;
    logf("BUSY -> %s", busyStable == LOW ? "LOW (playing)" : "HIGH (idle)");
  }
  if (playRequested && !started && now - playAt >= SETTLE_MS && busyStable == LOW) {
    started = true;
    logf("track playing (BUSY LOW %lu ms after play)", now - playAt);
  }

  // Messages from the player (errors, track finished). available() only parses what's already received.
  if (player.available()) {
    uint8_t type = player.readType();
    uint16_t value = player.read();
    if (type == DFPlayerError) {
      const char* reason = value == 5 ? "file index out of bound (no such track)"
                         : value == 6 ? "cannot find file"
                         : value == 2 ? "card sleeping"
                         : value == 4 ? "checksum mismatch" : "error";
      logf("player error %u: %s", value, reason);
    } else if (type == DFPlayerPlayFinished) {
      logf("player says track %u finished", value);
    } else if (type == DFPlayerCardRemoved) {
      logf("SD card removed");
    } else if (type == DFPlayerCardInserted || type == DFPlayerCardOnline) {
      logf("SD card ready");
    }
  }
}

bool playTrack(uint16_t track) {
  if (!ready()) return false;
  player.play(track);
  playAt = millis();
  playRequested = true;
  started = false;
  logf("play track %u", track);
  return true;
}

void stop() {
  if (!ready()) return;
  player.stop();
  playRequested = false;
  logf("stop");
}

// Applied by the next update(), or at init if the player isn't set up yet.
void setVolume(uint8_t volume) {
  wantedVolume = volume > 30 ? 30 : volume;
}

bool isPlaying() {
  if (!ready()) return false;
  if (playRequested && millis() - playAt < SETTLE_MS) return true;  // just asked to play: assume it is
  return busyStable == LOW;
}

bool trackStarted() {
  return ready() && playRequested && started;
}

unsigned long msSincePlay() {
  return millis() - playAt;
}

}  // namespace audio
