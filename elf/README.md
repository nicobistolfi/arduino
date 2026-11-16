# elf

A motion-triggered show. When the HC-SR501 PIR sensor detects motion, a light (3 LEDs on 9V, switched by a PN2222 transistor) turns on. 0.7 seconds later, an elf animatronic starts kicking and a sound starts playing on a Mac. When the sound ends, the legs stop and the light goes off at the same moment. Then motion is ignored for a 5-second cooldown. Every step prints to the serial monitor at 115200 baud with a timestamp.

Runs on an **ESP32 Dev Module** (30-pin, CP2102 USB). It was first built on an ELEGOO UNO R3.

The three LEDs stand in for a 12V spotlight in the final build. The elf kicks while a relay holds down its *try me* button. The sound plays on a DFPlayer Mini with its own speaker, and/or on a Mac through `kick-sound.sh`, cued over MQTT. Don't run both, or the sound plays twice.

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
- DFPlayer Mini (MP3-TF-16P V3.0 clone), a microSD card (FAT32) and a small speaker (3 W max)
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
- The relay's contacts are wired in parallel with the elf's *try me* button. Closing the relay is the same as pressing the button. The toy runs one routine per press and stops on its own even if the button stays held, so during a show the relay re-presses it every 5 s (see the show timeline below).
- GPIO25 is the ESP32 equivalent of D7 in the UNO version. Never use GPIO6–11 on the ESP32: they're connected to the flash chip.
- A bare relay coil needs a flyback diode across it (for example a 1N4007, stripe toward 5V), or switching it off can damage the transistor. Relay *modules* usually have one built in. A 10 kΩ resistor from the transistor's base to GND keeps the relay open while the ESP32 boots.

**DFPlayer Mini**

| DFPlayer pin | ESP32 pin |
|---|---|
| VCC | VIN (5V) |
| GND | GND |
| RX | 1 kΩ → GPIO17 (UART2 TX) |
| TX | GPIO16 (UART2 RX) |
| BUSY | GPIO4 (LOW while playing) |
| SPK_1 / SPK_2 | Speaker |

GPIO4 has the internal pull-up on, so an unconnected BUSY wire reads as "idle".

**SD card:** FAT32, files in the root folder, named `0001.mp3`, `0002.mp3`, and so on. The DFPlayer plays "track *n*" as the *n*-th file *copied* to the card, not by file name. Start from an empty card and copy the files one at a time, in order. Current tracks:

| Track | File | Sound |
|---|---|---|
| 1 | `0001.mp3` | Elf intro, the same sound as `001.mp3` (14.3 s) |

## Behavior

### The show

Every show runs on one clock, starting when the light begins to turn on (t = 0):

```
t = 0                        light starts fading up (over LEAD_MS = 0.7 s)
t = 0.7 s − AUDIO_LAG_MS     "play" sent on elf/audio, and the show's track started on the DFPlayer,
                             so the sound starts at t = 0.7 s
t = 0.7 s                    relay closes and holds the elf's button: legs start kicking, sound starts
every KICK_MS (5 s) after    relay lets go for KICK_GAP_MS (100 ms) and presses again, restarting the
                             toy's kick routine before it stops on its own (barely noticeable)
t = 0.7 s + SHOW_MS          relay opens and light goes off: legs, light and sound all end together
   (or: when the DFPlayer track ends)
```

**With a DFPlayer, the track sets the show's length.** The show runs while BUSY says the track is playing, and ends (legs, light) when it stops. BUSY is debounced (20 ms) and isn't trusted for the first 100 ms after `play`. The show falls back to `SHOW_MS`, and stops the DFPlayer at the end, in these cases:

- no DFPlayer was found at boot;
- the sequence has no track;
- BUSY doesn't go LOW within 1 s of `play` (missing file, or the BUSY wire isn't connected).

A BUSY stuck LOW ends the show after 2 minutes (`MAX_TRACK_MS`).

The track for each kind of show is set in one table at the top of `elf.ino`. The sequence is the show's trigger: `motion` (PIR), `manual` (`elf/light/set ON`) or `test` (serial `t`). Track 0 means no sound.

```cpp
const SequenceTrack SEQUENCE_TRACKS[] = {
  { "motion", 1 },  // 0001.mp3, elf intro
  { "manual", 1 },  // 0001.mp3, elf intro
  { "test",   1 },  // 0001.mp3, elf intro
};
```

Volume defaults to `AUDIO_VOLUME` (25, range 0–30), also at the top of `elf.ino`. Change it at runtime with `elf/config/set` `{"volume":30}`. It takes effect immediately, even during a show. The player is set up in the background 1.5 s after boot, which is the DFPlayer's start-up time. `begin()` with ACK is tried first, then without ACK and with a reset (clone boards often don't ACK). Presence is confirmed by asking for the volume. If no player answers, the serial log says `DFPlayer NOT found` and everything else runs without it. It keeps looking every 10 s in the background, so fixing the wiring doesn't need a restart. `audio: DFPlayer found` appears when it answers. The audio code is in `audio.h` / `audio.cpp`.

`SHOW_MS` (14340) is exactly the length of `001.mp3`, which is trimmed so the sound starts and ends right at the edges of the file. Each step is timed from the show's start, not from the previous step, so small delays in `loop()` never add up.

| State | What happens | Next |
|---|---|---|
| `WARMUP` | 60 s for the PIR to settle. Light off, relay open. | Prints `ARMED`, goes to `IDLE` |
| `IDLE` | Waits for the PIR to go HIGH (when mode is `AUTO`). | Prints `MOTION`, goes to `LEAD_IN` |
| `LEAD_IN` | Light fades up over `LEAD_MS`. Sends the sound cue `AUDIO_LAG_MS` before the end. | `SHOW` |
| `SHOW` | Light full, elf's button held (re-pressed every 5 s, legs kicking), sound playing, for `SHOW_MS`. Motion is ignored. | Light off, relay open, `stop` sent; `COOLDOWN` |
| `COOLDOWN` | Motion ignored for `COOLDOWN_MS` (5 s). | `IDLE` |

The relay is closed only in `SHOW`. Entering any other state opens it, and so does `RESTART`. It's open at boot and during the warm-up.

### Timings

| Constant | `config/set` key | Default | Meaning |
|---|---|---|---|
| `LEAD_MS` | `lead_ms` | 700 | Light comes on this long before the legs and sound start |
| `SHOW_MS` | `show_ms` | 14340 | Legs and sound duration. **Must match the sound file's length.** |
| `AUDIO_LAG_MS` | `audio_lag_ms` | 40 | Delay from sending `play` to sound coming out of the Mac. The cue is sent this much early. |
| `COOLDOWN_MS` | `cooldown_ms` | 5000 | Motion ignored after a show |
| `AUDIO_VOLUME` | `volume` | 25 | DFPlayer volume, 0–30. The only key that can change during a show. |
| `KICK_MS` | (none) | 5000 | During the show, the button is held this long before each re-press |
| `KICK_GAP_MS` | (none) | 100 | How long the button is let go between presses |

`KICK_MS` and `KICK_GAP_MS` are set in the sketch only. Change them and re-upload. The re-press cycle is timed from the moment the legs start, so it doesn't drift, and the last press is simply cut off when the show ends.

**Tuning the sync:** if the sound starts noticeably *after* the legs, raise `audio_lag_ms`; if it starts *before*, lower it. 40 ms was measured with the MacBook's built-in speakers: about 10 ms for MQTT and 4–45 ms for the player to start. **Bluetooth speakers or headphones add roughly 150–250 ms**, so use about 250 with them.

If you replace the sound, trim the silence from both ends and set `show_ms` to the new length. `afinfo file.mp3` shows the length, and `kick-player` prints it when it loads.

### Serial log of a show

```
[60.000s] ARMED
[72.416s] MOTION
[72.416s] SHOW START (motion)
[72.416s] state -> LEAD_IN
[73.076s] audio -> play
[73.116s] state -> SHOW
[73.116s] ELF KICK (motion)
[73.117s] elf -> HELD
[78.116s] elf -> gap
[78.216s] elf -> HELD again (re-press 1)
[83.216s] elf -> gap
[83.316s] elf -> HELD again (re-press 2)
[87.456s] elf -> RELEASED
[87.456s] audio -> stop
[87.456s] SHOW END, LIGHT OFF
[87.456s] state -> COOLDOWN
[92.456s] state -> IDLE
```

### Testing without the PIR

Type a letter in the serial monitor and press Enter:

| Key | What it does |
|---|---|
| `t` | Starts a full show right away (light, legs, sound), without the PIR. Its sequence is `test` in the track table. Works during the warm-up too, and is ignored while a show is running. |
| `r` | Turns the motion sensor off, or back on. It's the same as `elf/mode/set OFF` / `AUTO` and updates `elf/mode`. A running show isn't stopped. A retained `elf/mode/set` on the broker overrides it on the next reconnect. |
| `k` | Fires one short elf press (`ELF_PULSE_MS`, 300 ms), which you'll hear as a relay click. Ignored during the 60-second warm-up. |

Publishing `ON` to `elf/light/set` also runs a full show (see below).

If someone is still in front of the sensor when the cooldown ends, a new show starts right away, because `IDLE` reacts to the PIR being HIGH, not to it changing.

The board's on-board blue LED (GPIO2) mirrors the raw PIR output, and `PIR: HIGH` / `PIR: LOW` lines are logged for debugging.

Pins and timings are named constants at the top of `elf.ino`. All timing uses `millis()`, so `loop()` never blocks.

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
| `elf/state` | `WARMUP` `IDLE` `LEAD_IN` `SHOW` `COOLDOWN` | yes | Every state change |
| `elf/light` | `ON` / `OFF` | yes | When a show starts, and when it ends |
| `elf/audio` | `play` / `stop` | no | Sound cues for `kick-sound.sh`: `play` just before the legs start, `stop` when the show ends. Sent at QoS 0 and immediately, not queued, because the timing matters. |
| `elf/motion` | `1` / `0` | yes | Every PIR change |
| `elf/mode` | `AUTO` / `OFF` | yes | On connect and when the mode changes |
| `elf/config` | `{"lead_ms":700,"show_ms":14340,"audio_lag_ms":40,"cooldown_ms":5000,"volume":25}` | yes | On connect and when the config changes |
| `elf/info` | `{"ip":"…","rssi":-58,"client_id":"…","uptime_s":3}` | yes | On connect |
| `elf/kick` | `motion` / `manual` / `serial test` | no | When the legs start kicking (once per show), or on a `k` test press |
| `elf/pong` | `{"uptime_s":…,"rssi":…,"state":"…"}` | no | In reply to `PING` |

Every retained topic is republished on each connect, so a new subscriber always sees the current values.

`kick-sound.sh` on the Mac also publishes one topic:

| Topic | Payload | Retained | When |
|---|---|---|---|
| `elf/intro/end` | `end` | no | When `001.mp3` has played to its end, about 15 s into the show. Not sent if the show is cut short and the sound is stopped. |

### Commands (elf subscribes)

| Topic | Payload | Effect |
|---|---|---|
| `elf/light/set` | `ON` | Start a full show now. This works in any state, even in mode `OFF` or during warm-up, and is ignored if a show is already running. |
| `elf/light/set` | `OFF` | Cut a running show short. Light off, legs stop and the sound stops, together. Then the normal cooldown. |
| `elf/mode/set` | `AUTO` / `OFF` | `OFF` stops motion from starting shows, and `AUTO` turns it back on. A show that's already running finishes. |
| `elf/config/set` | `{"lead_ms":700,"show_ms":14340,"audio_lag_ms":40,"cooldown_ms":5000,"volume":25}` | Change the timings and volume (see [Timings](#timings)). Any subset of the keys works. Limits: `lead_ms` 0–5000, `show_ms` 1000–600000, `audio_lag_ms` 0–`lead_ms`, `cooldown_ms` 0–600000, `volume` 0–30. Invalid values are rejected, and nothing changes. Timing changes are also ignored while a show is running, so a show can never fall out of sync with its sound. `volume` applies right away, even during a show. |
| `elf/cmd` | `PING` | Reply on `elf/pong` |
| `elf/cmd` | `RESTART` | Publish `offline` and reboot |

Payloads are case-insensitive. Each received message is logged to serial, and anything invalid is logged and ignored.

Publish `mode/set` and `config/set` with **retain** (`-r`) to keep them across reboots, since elf receives retained messages again each time it connects. The broker keeps only the *last* retained message on a topic, so put every setting you want kept in one `config/set` message, for example `{"volume":30,"audio_lag_ms":60}`. Don't retain `light/set` or `cmd`. Elf ignores retained copies of those, so a retained `RESTART` can't cause a reboot loop.

### Try it

With [Mosquitto](https://mosquitto.org/) installed (`brew install mosquitto`), on any machine that can reach the broker:

```sh
# watch everything
mosquitto_sub -h 192.168.1.10 -t 'elf/#' -v

# send commands
mosquitto_pub -h 192.168.1.10 -t elf/light/set -m ON
mosquitto_pub -h 192.168.1.10 -t elf/light/set -m OFF
mosquitto_pub -h 192.168.1.10 -t elf/mode/set -m OFF -r
mosquitto_pub -h 192.168.1.10 -t elf/config/set -m '{"volume":30}' -r
mosquitto_pub -h 192.168.1.10 -t elf/cmd -m PING
```

Add `-u <user> -P <password>` if the broker has a login.

### Sound on a Mac

`kick-sound.sh` plays `001.mp3` on the Mac in sync with the show:

```sh
./kick-sound.sh      # Ctrl+C (or kill) to stop
```

- It listens on `elf/audio` and passes each `play` / `stop` to `kick-player`, a small Swift program (`kick-player.swift`) that keeps the sound loaded in memory and starts it within a few milliseconds. Launching `afplay` for each sound took 300–800 ms, and varied too much to stay in sync.
- The script builds `kick-player` into `build/` on first run, and again whenever the source changes. This needs `swiftc`, from the Xcode Command Line Tools.
- When the sound plays to its end, it publishes `end` on `elf/intro/end` (QoS 1, not retained). A sound stopped early, because the show was cut short, sends nothing.
- It reads the broker settings from `secrets.h`. The MQTT login is passed to `mosquitto_sub` and `mosquitto_pub` through a private, temporary options file, not on the command line, where `ps` would show the password.
- If the broker is unreachable, it retries every 5 seconds. Environment variables override the defaults, for example `SOUND=other.mp3 ./kick-sound.sh` or `END_TOPIC=…`. It needs `brew install mosquitto`.

Measured end to end, the sound started about 13 ms after the legs, using the built-in speakers.

The ESP32 turns off WiFi power saving (`WiFi.setSleep(false)`), because modem sleep delayed the sound cue by up to half a second.

### Troubleshooting

- **WiFi never connects:** check the network name and password (both are case-sensitive) and that the network is 2.4 GHz. Signal weaker than about -80 dBm is unreliable.
- **`track didn't start (BUSY never went LOW)`** but you hear the sound: the BUSY wire isn't reaching GPIO4. With it working, the log shows `audio: BUSY -> LOW (playing)` right after `play track`.
- **`no DFPlayer: show timed by show_ms`** at the start of a show: the player hasn't been found (see the next item). The show runs on the `show_ms` timer, without sound.
- **`DFPlayer NOT found`:** check that TX and RX are crossed (DFPlayer TX → GPIO16), the 1 kΩ on RX, 5V on VCC, and that a FAT32 card is inserted.
- **`player error 6: cannot find file`** or **`5: file index out of bound`:** the track number is larger than the number of files on the card.
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

### Libraries

| Library | Version | Install |
|---|---|---|
| DFRobotDFPlayerMini | 1.0.6 | `arduino-cli lib install DFRobotDFPlayerMini@1.0.6` |

WiFi and the MQTT client come with the ESP32 core.

If the upload stalls at `Connecting...`, hold the board's **BOOT** button until it starts writing.
