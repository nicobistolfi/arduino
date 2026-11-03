// Hello World: blinks the built-in LED and prints to the serial monitor.

const unsigned long BLINK_INTERVAL_MS = 1000;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(9600);
  Serial.println("Hello, World!");
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.println("LED on");
  delay(BLINK_INTERVAL_MS);

  digitalWrite(LED_BUILTIN, LOW);
  Serial.println("LED off");
  delay(BLINK_INTERVAL_MS);
}
