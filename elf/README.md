# elf

Turns on the board's built-in LED (pin 13) whenever the HC-SR501 PIR sensor detects motion, and prints `Motion detected` / `Motion ended` to the serial monitor at 9600 baud.

## Hardware

From the [ELEGOO UNO R3 Most Complete Starter Kit](https://www.elegoo.com/):

- ELEGOO UNO R3 board
- HC-SR501 PIR motion sensor
- 3 female-to-male jumper wires

## Wiring

| HC-SR501 pin | UNO pin |
|---|---|
| VCC | 5V |
| OUT | D6 |
| GND | GND |

With the dome removed, the pins are labeled on the sensor's board. The middle pin is OUT.

For the wiring diagram, see lesson **2.15 HC-SR501** in the kit's tutorials (`English/Part 2/2.15 HR-SR501 UNO.pdf`).

## Sensor tuning

- **Warm-up:** after you power it on, the sensor needs about 1 minute to settle. It may trigger randomly during that time.
- **Sensitivity knob:** sets the detection range, about 3–7 m.
- **Time-delay knob:** sets how long OUT stays HIGH after motion, about 3 s to 5 min. Turn it fully counter-clockwise for the shortest time.
- **Trigger jumper:** `H` (repeat) keeps the output HIGH while motion continues. `L` (single) goes LOW after the delay even if motion continues. `H` works best for this project.

## Build and upload

From this folder:

```sh
arduino-cli board list                       # find your port
arduino-cli compile -u -p /dev/cu.usbmodem101
arduino-cli monitor -p /dev/cu.usbmodem101 -c baudrate=9600
```
