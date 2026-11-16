// kick-player: preloads one audio file and plays it with minimal delay on command.
//
// Reads commands from stdin, one per line:
//   play  start the sound from the beginning (restarts it if already playing)
//   stop  stop the sound
// Built and run by kick-sound.sh, which feeds it the payloads of the elf/audio MQTT topic.
// When a sound plays to its end (not when it is stopped), it prints a line ending in
// "finished", which kick-sound.sh turns into an elf/intro/end message.
//
// Unlike launching afplay per sound (300-800 ms and varying), the file is decoded and the
// audio output prepared once at startup, so "play" starts within a few milliseconds.

import AVFoundation
import Foundation

setvbuf(stdout, nil, _IOLBF, 0)

let clock: DateFormatter = {
  let f = DateFormatter()
  f.dateFormat = "HH:mm:ss.SSS"
  return f
}()

func log(_ message: String) {
  print("[\(clock.string(from: Date()))] \(message)")
}

guard CommandLine.arguments.count == 2 else {
  FileHandle.standardError.write("usage: kick-player <audio file>\n".data(using: .utf8)!)
  exit(2)
}

let url = URL(fileURLWithPath: CommandLine.arguments[1])
let player: AVAudioPlayer
do {
  player = try AVAudioPlayer(contentsOf: url)
} catch {
  log("cannot open \(url.path): \(error.localizedDescription)")
  exit(1)
}

// Warm up the audio output with a short silent play: the very first play() is otherwise slower.
player.volume = 0
player.play()
Thread.sleep(forTimeInterval: 0.3)
player.stop()
player.volume = 1
player.currentTime = 0
player.prepareToPlay()
log(String(format: "loaded %@ (%.3f s), ready", url.lastPathComponent, player.duration))

// True from "play" until the sound ends or is stopped. AVAudioPlayer's delegate callback
// never arrives in this run-loop-only program, so a timer watches isPlaying instead.
var playing = false
Timer.scheduledTimer(withTimeInterval: 0.01, repeats: true) { _ in
  if playing && !player.isPlaying {
    playing = false
    player.currentTime = 0
    player.prepareToPlay()
    log("finished")
  }
}

DispatchQueue.global().async {
  while let line = readLine() {
    let command = line.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    DispatchQueue.main.async {
      switch command {
      case "play":
        if player.isPlaying { player.stop() }
        player.currentTime = 0
        let started = DispatchTime.now().uptimeNanoseconds
        player.play()
        playing = true
        let ms = Double(DispatchTime.now().uptimeNanoseconds - started) / 1_000_000
        log(String(format: "play (started in %.1f ms)", ms))
      case "stop":
        playing = false
        if player.isPlaying {
          player.stop()
          log("stop")
        }
        player.currentTime = 0
        player.prepareToPlay()
      default:
        log("ignored: \(line)")
      }
    }
  }
  DispatchQueue.main.async { exit(0) }  // stdin closed: the MQTT listener exited
}

RunLoop.main.run()
