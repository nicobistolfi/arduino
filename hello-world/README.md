# hello-world

The first project in this repo. It blinks the board's built-in LED once per second and prints `Hello, World!` and the LED state to the serial monitor at 9600 baud.

## Hardware

- Arduino Uno (or any board with a `LED_BUILTIN`)
- USB cable

No extra wiring is needed.

## Build and upload

From this folder:

```sh
arduino-cli compile
arduino-cli board list                      # find your port, e.g. /dev/cu.usbmodem1101
arduino-cli upload -p /dev/cu.usbmodem1101
arduino-cli monitor -p /dev/cu.usbmodem1101 -c baudrate=9600
```

The board (`arduino:avr:uno`) is set in `sketch.yaml`. To use a different board, edit that file or pass `--fqbn`.
