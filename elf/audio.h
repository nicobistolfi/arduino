// Audio: DFPlayer Mini (MP3-TF-16P V3.0 and clones) on UART2, with the playing state read from BUSY.
//
// Non-blocking from loop()'s point of view:
//   - The player is initialized in a short-lived background task, after the DFPlayer's boot time,
//     because the library's begin() and its presence check wait on the serial line for up to ~3 s.
//   - isPlaying() reads the BUSY pin (debounced), never the serial line.
//   - Each command (play, stop, volume) costs ~10 ms inside the library (its fixed post-send pause).
// If no player is found, every call is a harmless no-op and ready() stays false.

#pragma once
#include <Arduino.h>

namespace audio {

// Starts UART2 and schedules the player's init in the background. Call once from setup().
void begin(int rxPin, int txPin, int busyPin, uint8_t volume);

// Debounces BUSY and logs messages from the player. Call every loop().
void update();

bool ready();                    // player found and initialized
bool playTrack(uint16_t track);  // plays 000<track>.mp3 (see the SD card notes in elf.ino); false if not ready
void stop();
void setVolume(uint8_t volume);  // 0..30; sent on the next update() (or at init, if before it)
bool isPlaying();                // BUSY LOW (debounced); true for the first AUDIO_SETTLE_MS after playTrack()
bool trackStarted();             // BUSY confirmed the last playTrack() actually started
unsigned long msSincePlay();     // time since the last playTrack()

}  // namespace audio
