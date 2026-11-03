# elf

A motion-activated light. When the HC-SR501 PIR sensor detects motion, a light (3 LEDs on 9V, switched by a PN2222 transistor) fades on, stays on for 10 seconds, fades off, then ignores motion for a 5-second cooldown. Every step prints to the serial monitor at 9600 baud with a timestamp.

The three LEDs stand in for a 12V spotlight in the final build. An elf relay on D7 comes next.

## Hardware

From the [ELEGOO UNO R3 Most Complete Starter Kit](https://www.elegoo.com/):

- ELEGOO UNO R3 board and 9V adapter (barrel jack)
- HC-SR501 PIR motion sensor
- PN2222 transistor
- Resistors: 1 kΩ, 10 kΩ, 3 × 330 Ω
- 3 LEDs
- Breadboard and jumper wires

## Wiring

**PIR sensor**

| HC-SR501 pin | UNO pin |
|---|---|
| VCC | 5V |
| OUT | D2 |
| GND | GND |

Set the PIR jumper to `H` and turn the time-delay knob to its minimum, fully counter-clockwise. The code controls how long the light stays on.

**Light driver** (low-side switch)

- UNO `VIN` → bottom (+) breadboard rail. This is the **9V rail**; keep the top rail for 5V.
- UNO `GND` → bottom (−) rail, and jumper it to the top (−) rail.
- PN2222, flat side facing you, pins **E · B · C** from left to right. Emitter → (−) rail.
- `D6` → 1 kΩ → base. Add a 10 kΩ resistor from the base to the (−) rail, which keeps the LEDs off while the board boots.
- Three branches: 9V rail → 330 Ω → LED long leg. Connect all three LED short legs to the collector.

VIN carries about 9V only when the adapter is plugged into the barrel jack. On USB power alone it's about 4.5V. Never let the 9V rail touch an UNO pin or the 5V rail. Only the transistor connects the two, plus the shared GND.

In the final build, a 12V supply's (+) replaces VIN, and its (−) joins the same GND rail.

## Behavior

| State | What happens | Next |
|---|---|---|
| `WARMUP` | 60 s for the PIR to settle. Light off. | Prints `ARMED`, goes to `IDLE` |
| `IDLE` | Waits for the PIR to go HIGH. | Prints `MOTION`, goes to `FADING_IN` |
| `FADING_IN` | Light ramps 0 → full over 1 s. | `ON` |
| `ON` | Light at full until 10 s after the motion was detected. More motion doesn't extend it. | `FADING_OUT` |
| `FADING_OUT` | Light ramps full → 0 over 1 s. | Prints `LIGHT OFF`, goes to `COOLDOWN` |
| `COOLDOWN` | Motion ignored for 5 s. | `IDLE` |

If someone is still in front of the sensor when the cooldown ends, the light comes back on right away, because `IDLE` reacts to the PIR being HIGH, not to it changing.

The board's built-in LED (pin 13) mirrors the raw PIR output, and `PIR: HIGH` / `PIR: LOW` lines are logged for debugging.

Pins and timings are named constants at the top of `elf.ino`. All timing uses `millis()`, so `loop()` never blocks.

Example output:

```
[60.000s] ARMED
[60.001s] state -> IDLE
[72.415s] PIR: HIGH
[72.416s] MOTION
[72.418s] state -> FADING_IN
[73.418s] state -> ON
[82.416s] state -> FADING_OUT
[83.416s] LIGHT OFF
[83.418s] state -> COOLDOWN
[88.418s] state -> IDLE
```

## Build and upload

From this folder:

```sh
arduino-cli board list                       # find your port
arduino-cli compile -u -p /dev/cu.usbmodem101
arduino-cli monitor -p /dev/cu.usbmodem101 -c baudrate=9600
```

Keep USB plugged in for uploading. The 9V adapter can stay plugged in at the same time.
