// Motion sensor: turns on the built-in LED while the HC-SR501 PIR sensor detects motion.

const int PIR_PIN = 6;  // HC-SR501 OUT pin (same pin as the ELEGOO lesson 2.15 wiring)

int lastState = LOW;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(PIR_PIN, INPUT);
  digitalWrite(LED_BUILTIN, LOW);
  Serial.begin(9600);
  Serial.println("Motion sensor ready (allow ~1 minute for the PIR to settle)");
}

void loop() {
  int state = digitalRead(PIR_PIN);
  digitalWrite(LED_BUILTIN, state);

  if (state != lastState) {
    Serial.println(state == HIGH ? "Motion detected" : "Motion ended");
    lastState = state;
  }
}
