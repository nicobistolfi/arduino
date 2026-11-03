# hello-world

The first project in this repo. It blinks the board's built-in LED once per second and prints `Hello, World!` and the LED state to the serial monitor at 9600 baud.

## Hardware

This project uses the [ELEGOO UNO R3 Most Complete Starter Kit](https://www.elegoo.com/):

- ELEGOO UNO R3 board (Arduino Uno compatible)
- USB cable (included in the kit)

No extra wiring is needed. The sketch uses the board's built-in LED on pin 13.

For more on blinking an LED, see lesson **2.1 LED** in the kit's tutorials (`English/Part 2/2.1 LED -UNO.pdf` in the kit download).

## Build and upload

From this folder:

```sh
arduino-cli compile
arduino-cli board list                      # find your port, e.g. /dev/cu.usbmodem1101
arduino-cli upload -p /dev/cu.usbmodem1101
arduino-cli monitor -p /dev/cu.usbmodem1101 -c baudrate=9600
```

The board (`arduino:avr:uno`) is set in `sketch.yaml`. To use a different board, edit that file or pass `--fqbn`.
