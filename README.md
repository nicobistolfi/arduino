# Arduino projects

A collection of Arduino sketches, built and uploaded with [arduino-cli](https://arduino.github.io/arduino-cli/).

## Layout

Each project lives in its own top-level folder. The folder name, the main `.ino` file and the sketch name must match (this is an Arduino rule):

```
arduino/
├── README.md              # this file
└── hello-world/
    ├── README.md          # what the project does, hardware, wiring, how to run it
    ├── hello-world.ino    # main sketch, same name as the folder
    └── sketch.yaml        # default board (FQBN) and other settings for this project
```

Every project **must** have:

- **`<project>.ino`**, the main sketch. Extra `.ino`, `.h` or `.cpp` files can sit next to it.
- **`README.md`**, which covers what the project does, the board and parts used, the wiring, and how to build and run it.
- **`sketch.yaml`**, which sets the target board, so `arduino-cli compile` and `upload` work without flags.

Never commit passwords or API keys. Put them in a `secrets.h` file, which `.gitignore` excludes, and commit a `secrets.example.h` with placeholder values next to it. See `elf/` for an example.

## One-time setup

```sh
brew install arduino-cli
arduino-cli core update-index
arduino-cli core install arduino:avr       # Uno, Nano, Mega, ...
```

On Apple Silicon Macs, the AVR compiler needs Rosetta:

```sh
softwareupdate --install-rosetta --agree-to-license
```

For ESP32 boards, add Espressif's board list and install the ESP32 core:

```sh
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

ESP32 boards with a CP2102 USB chip show up as `/dev/cu.usbserial-*`, and `board list` reports them as `Unknown`. Their FQBN is `esp32:esp32:esp32` (ESP32 Dev Module).

For other boards, install their core. Run `arduino-cli core search <name>` to find it.

## Create a new project

```sh
arduino-cli sketch new my-project
cd my-project
echo "default_fqbn: arduino:avr:uno" > sketch.yaml
```

Then write the code in `my-project.ino` and add a `README.md` (copy `hello-world/README.md` as a starting point). Use lowercase, hyphen-separated folder names.

To find the FQBN for your board, plug it in and run `arduino-cli board list`, or run `arduino-cli board listall`.

## Build, upload, monitor

Run these from inside the project folder:

```sh
arduino-cli compile                               # build
arduino-cli board list                            # find the port
arduino-cli upload -p /dev/cu.usbmodemXXXX        # flash
arduino-cli monitor -p /dev/cu.usbmodemXXXX -c baudrate=9600
```

Or compile and upload in one step: `arduino-cli compile -u -p /dev/cu.usbmodemXXXX`.

## Libraries

```sh
arduino-cli lib search <name>
arduino-cli lib install "<Library Name>"
```

List each library a project needs, with its version, in that project's README.

## Projects

| Project | Board | Description |
|---|---|---|
| [hello-world](hello-world/) | Arduino Uno | Blinks the built-in LED and prints to serial |
| [motion-sensor](motion-sensor/) | Arduino Uno | Lights the built-in LED when the HC-SR501 PIR sensor detects motion |
| [elf](elf/) | ESP32 Dev Module | Motion-activated light and kicking elf animatronic (relay), with WiFi, MQTT telemetry and remote commands |
