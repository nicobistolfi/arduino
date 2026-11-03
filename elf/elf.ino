// Elf: when the HC-SR501 PIR sensor detects motion, fade a light on (driven by a
// PN2222 transistor on D6), hold it for 10 s, fade it out, then cool down.
// Fully non-blocking: all timing uses millis(), so more outputs can be added to loop().

// ---- Pins ----
const int PIR_PIN   = 2;  // HC-SR501 OUT (jumper on H, time knob at minimum)
const int LIGHT_PIN = 6;  // PWM -> 1k -> PN2222 base; HIGH = LEDs on

// ---- Timings (ms) ----
const unsigned long WARMUP_MS   = 60000;  // PIR settle time after power-on
const unsigned long LIGHT_ON_MS = 10000;  // light stays on this long from the moment motion is detected
const unsigned long FADE_MS     = 1000;   // fade-in and fade-out duration
const unsigned long COOLDOWN_MS = 5000;   // ignore motion this long after the light turns off

enum State { WARMUP, IDLE, FADING_IN, ON, FADING_OUT, COOLDOWN };
const char* const STATE_NAMES[] = { "WARMUP", "IDLE", "FADING_IN", "ON", "FADING_OUT", "COOLDOWN" };

State state = WARMUP;
unsigned long stateStartedAt = 0;  // when the current state began
unsigned long motionAt = 0;        // when the motion that turned the light on was detected
int lastPir = LOW;

void printTimestamp() {
  unsigned long now = millis();
  Serial.print('[');
  Serial.print(now / 1000);
  Serial.print('.');
  unsigned long ms = now % 1000;
  if (ms < 100) Serial.print('0');
  if (ms < 10) Serial.print('0');
  Serial.print(ms);
  Serial.print("s] ");
}

void logLine(const char* message) {
  printTimestamp();
  Serial.println(message);
}

void enterState(State next) {
  state = next;
  stateStartedAt = millis();
  printTimestamp();
  Serial.print("state -> ");
  Serial.println(STATE_NAMES[next]);
}

void setup() {
  pinMode(PIR_PIN, INPUT);
  pinMode(LIGHT_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  analogWrite(LIGHT_PIN, 0);

  Serial.begin(9600);
  logLine("Elf starting, PIR warming up for 60 s");
  enterState(WARMUP);
}

void loop() {
  unsigned long now = millis();
  unsigned long elapsed = now - stateStartedAt;

  // Sensor debug: the built-in LED mirrors the raw PIR output, and edges are logged.
  int pir = digitalRead(PIR_PIN);
  digitalWrite(LED_BUILTIN, pir);
  if (pir != lastPir) {
    logLine(pir == HIGH ? "PIR: HIGH" : "PIR: LOW");
    lastPir = pir;
  }

  switch (state) {
    case WARMUP:
      if (elapsed >= WARMUP_MS) {
        logLine("ARMED");
        enterState(IDLE);
      }
      break;

    case IDLE:
      if (pir == HIGH) {
        motionAt = now;
        logLine("MOTION");
        enterState(FADING_IN);
      }
      break;

    case FADING_IN:
      if (elapsed >= FADE_MS) {
        analogWrite(LIGHT_PIN, 255);
        enterState(ON);
      } else {
        analogWrite(LIGHT_PIN, elapsed * 255UL / FADE_MS);
      }
      break;

    case ON:
      // Timer runs from the original motion; further motion does not extend it.
      if (now - motionAt >= LIGHT_ON_MS) {
        enterState(FADING_OUT);
      }
      break;

    case FADING_OUT:
      if (elapsed >= FADE_MS) {
        analogWrite(LIGHT_PIN, 0);
        logLine("LIGHT OFF");
        enterState(COOLDOWN);
      } else {
        analogWrite(LIGHT_PIN, 255 - (int)(elapsed * 255UL / FADE_MS));
      }
      break;

    case COOLDOWN:
      if (elapsed >= COOLDOWN_MS) {
        enterState(IDLE);
      }
      break;
  }
}
