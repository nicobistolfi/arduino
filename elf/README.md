# elf

A motion-activated light. When the HC-SR501 PIR sensor detects motion, a light (3 LEDs on 9V, switched by a PN2222 transistor) fades on, stays on for 10 seconds, fades off, then ignores motion for a 5-second cooldown. Every step prints to the serial monitor at 115200 baud with a timestamp.

Runs on an **ESP32 Dev Module** (30-pin, CP2102 USB). It was first built on an ELEGOO UNO R3.

The three LEDs stand in for a 12V spotlight in the final build. Once the light is fully on, a relay "presses" the elf animatronic's *try me* button once, so the elf kicks.

## Hardware

- ESP32 Dev Module (30-pin, CP2102 USB)
- Separate supply for the LEDs, such as the kit's 9V adapter on a barrel-jack breadboard adapter, or a 9V battery

From the [ELEGOO UNO R3 Most Complete Starter Kit](https://www.elegoo.com/):

- HC-SR501 PIR motion sensor
- PN2222 transistor
- Resistors: 1 kΩ, 10 kΩ, 3 × 330 Ω
- 3 LEDs
- Second PN2222 and a 5V relay (for the elf)
- Elf animatronic with a momentary *try me* button
- Breadboard and jumper wires

## Wiring

**PIR sensor**

| HC-SR501 pin | ESP32 pin |
|---|---|
| VCC | VIN (5V from USB) |
| OUT | GPIO27 (D27) |
| GND | GND |

The sensor runs on 5V, but its OUT signal is 3.3V, which is safe for the ESP32.

Set the PIR jumper to `H` and turn the time-delay knob to its minimum, fully counter-clockwise. The code controls how long the light stays on.

**Light driver** (low-side switch)

- LED supply (+) → bottom (+) breadboard rail. This is the **9V rail**.
- LED supply (−) → bottom (−) rail. Connect ESP32 `GND` to the same rail; the grounds must be shared.
- PN2222, flat side facing you, pins **E · B · C** from left to right. Emitter → (−) rail.
- `GPIO26` (D26) → 1 kΩ → base. Add a 10 kΩ resistor from the base to the (−) rail, which keeps the LEDs off while the board boots.
- Three branches: 9V rail → 330 Ω → LED long leg. Connect all three LED short legs to the collector.

The ESP32 has no barrel jack, and its `VIN` pin only carries about 5V from USB, so the LEDs need their own supply. Never let the 9V rail touch any ESP32 pin: ESP32 pins are 3.3V and are damaged by anything higher. Only the transistor connects the two, plus the shared GND.

In the final build, a 12V supply replaces the 9V LED supply, and its (−) joins the same GND rail.

**Elf relay**

- `GPIO25` (D25) drives a second PN2222, which switches a 5V relay. `HIGH` closes the relay.
- The relay's contacts are wired in parallel with the elf's *try me* button. Closing the relay is the same as pressing the button, and one press plays one full kick cycle.
- GPIO25 is the ESP32 equivalent of D7 in the UNO version. Never use GPIO6–11 on the ESP32: they're connected to the flash chip.
- A bare relay coil needs a flyback diode across it (for example a 1N4007, stripe toward 5V), or switching it off can damage the transistor. Relay *modules* usually have one built in. A 10 kΩ resistor from the transistor's base to GND keeps the relay open while the ESP32 boots.

## Behavior

| State | What happens | Next |
|---|---|---|
| `WARMUP` | 60 s for the PIR to settle. Light off. | Prints `ARMED`, goes to `IDLE` |
| `IDLE` | Waits for the PIR to go HIGH. | Prints `MOTION`, goes to `FADING_IN` |
| `FADING_IN` | Light ramps 0 → full over 1 s. | `ON` |
| `ON` | Light at full. In a motion show, the elf is pressed once, `ELF_DELAY_MS` (default 0) after entering `ON`. The light stays on until 10 s after the motion was detected; more motion doesn't extend it. | `FADING_OUT` |
| `FADING_OUT` | Light ramps full → 0 over 1 s. | Prints `LIGHT OFF`, goes to `COOLDOWN` |
| `COOLDOWN` | Motion ignored for 5 s. | `IDLE` |

**Elf press rules:**
- Exactly one press per motion show. The relay closes for `ELF_PULSE_MS` (300 ms) and is released by a timer in `loop()`, never by `delay()`. A pulse that's already running is never extended.
- The relay stays open at boot and during the warm-up.
- A light turned on with `elf/light/set ON` doesn't press the elf. If the light is turned off before the press happens, the press is cancelled.
- Serial log: `ELF KICK (motion)`, `elf -> PRESSED`, then `elf -> RELEASED`.

**Testing the elf without the PIR:** type `k` in the serial monitor and press Enter. It fires one pulse, which you'll hear as a relay click, and the elf kicks. It's ignored during the 60-second warm-up.

If someone is still in front of the sensor when the cooldown ends, the light comes back on right away, because `IDLE` reacts to the PIR being HIGH, not to it changing.

The board's on-board blue LED (GPIO2) mirrors the raw PIR output, and `PIR: HIGH` / `PIR: LOW` lines are logged for debugging.

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

## WiFi and MQTT

The board connects to WiFi and then to an MQTT broker in the background. It publishes everything it does and accepts commands. The light keeps working with or without a network, and both connections retry on their own.

### Setup

```sh
cp secrets.example.h secrets.h   # only if secrets.h doesn't exist yet
```

In `secrets.h`, set:

- `WIFI_SSID` and `WIFI_PASSWORD`. The ESP32 only joins **2.4 GHz** networks.
- `MQTT_HOST` and `MQTT_PORT`, the broker's address. Port 1883 is plain MQTT.
- `MQTT_USER` and `MQTT_PASSWORD`. Leave both as `""` if the broker has no login.

`secrets.h` is in `.gitignore` and never gets committed. The sketch won't compile without it.

Serial output on a good connection:

```
[0.012s] WiFi connecting to my-network
[2.341s] WiFi connected to my-network, IP 192.168.1.42, signal -58 dBm
[2.342s] MQTT connecting to 192.168.1.10:1883 as elf-1A2B3C4D5E6F
[2.398s] MQTT connected
```

### Telemetry (elf publishes)

| Topic | Payload | Retained | When |
|---|---|---|---|
| `elf/status` | `online` / `offline` | yes | `online` on connect. `offline` is sent by the broker automatically if elf drops off (last will), or by elf before a `RESTART` |
| `elf/state` | `WARMUP` `IDLE` `FADING_IN` `ON` `FADING_OUT` `COOLDOWN` | yes | Every state change |
| `elf/light` | `ON` / `OFF` | yes | When the light starts fading on, and when it's fully off |
| `elf/motion` | `1` / `0` | yes | Every PIR change |
| `elf/mode` | `AUTO` / `OFF` | yes | On connect and when the mode changes |
| `elf/config` | `{"on_ms":10000,"fade_ms":1000,"cooldown_ms":5000}` | yes | On connect and when the config changes |
| `elf/info` | `{"ip":"…","rssi":-58,"client_id":"…","uptime_s":3}` | yes | On connect |
| `elf/kick` | `motion` / `serial test` | no | Each time the elf is pressed |
| `elf/pong` | `{"uptime_s":…,"rssi":…,"state":"…"}` | no | In reply to `PING` |

Every retained topic is republished on each connect, so a new subscriber always sees the current values.

### Commands (elf subscribes)

| Topic | Payload | Effect |
|---|---|---|
| `elf/light/set` | `ON` | Fade the light on now and keep it on until `OFF`. This works in any state, even during warm-up. |
| `elf/light/set` | `OFF` | Fade the light off now. It then goes through the normal cooldown. |
| `elf/mode/set` | `AUTO` / `OFF` | `OFF` stops motion from triggering the light, and `AUTO` turns it back on. A light that's already on finishes its cycle. |
| `elf/config/set` | `{"on_ms":20000,"fade_ms":1500,"cooldown_ms":5000}` | Change the timings. Any subset of the keys works. Limits: `on_ms` 1000–3600000, `fade_ms` 0–10000 and ≤ `on_ms`, `cooldown_ms` 0–600000. Invalid values are rejected, and nothing changes. |
| `elf/cmd` | `PING` | Reply on `elf/pong` |
| `elf/cmd` | `RESTART` | Publish `offline` and reboot |

Payloads are case-insensitive. Each received message is logged to serial, and anything invalid is logged and ignored.

Publish `mode/set` and `config/set` with **retain** (`-r`) to keep them across reboots, since elf receives retained messages again each time it connects. Don't retain `light/set` or `cmd`. Elf ignores retained copies of those, so a retained `RESTART` can't cause a reboot loop.

### Try it

With [Mosquitto](https://mosquitto.org/) installed (`brew install mosquitto`), on any machine that can reach the broker:

```sh
# watch everything
mosquitto_sub -h 192.168.1.10 -t 'elf/#' -v

# send commands
mosquitto_pub -h 192.168.1.10 -t elf/light/set -m ON
mosquitto_pub -h 192.168.1.10 -t elf/light/set -m OFF
mosquitto_pub -h 192.168.1.10 -t elf/mode/set -m OFF -r
mosquitto_pub -h 192.168.1.10 -t elf/config/set -m '{"on_ms":20000}' -r
mosquitto_pub -h 192.168.1.10 -t elf/cmd -m PING
```

Add `-u <user> -P <password>` if the broker has a login.

### Troubleshooting

- **WiFi never connects:** check the network name and password (both are case-sensitive) and that the network is 2.4 GHz. Signal weaker than about -80 dBm is unreliable.
- **MQTT never connects:** check `MQTT_HOST` and `MQTT_PORT`, that the broker accepts connections from the network (Mosquitto 2.x only listens on localhost unless configured otherwise), and the username and password.

## Build and upload

From this folder:

```sh
arduino-cli board list                       # find your port
arduino-cli compile -u -p /dev/cu.usbserial-0001
arduino-cli monitor -p /dev/cu.usbserial-0001 -c baudrate=115200,dtr=off,rts=off
```

Keep `dtr=off,rts=off` on the monitor. With the defaults, the CP2102's auto-reset lines hold the ESP32 in reset and the monitor shows only garbage. Press the board's **EN** button to restart the sketch while watching.

The board (`esp32:esp32:esp32`) is set in `sketch.yaml`. It needs the ESP32 core; see the root README.

If the upload stalls at `Connecting...`, hold the board's **BOOT** button until it starts writing.
