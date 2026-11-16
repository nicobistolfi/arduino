// Keeps the external display (the projector) black, and plays a random santa video on it,
// centered and scaled to cover the whole screen, each time a line arrives on stdin.
// projector.sh feeds it the messages of the elf/intro/end MQTT topic.
//
// Usage: projector-player [--now] video.mp4 [video2.mp4 ...]
//   --now   play one video right away (for testing without MQTT)
// Quit with Esc or Cmd+Q (exit 0). If stdin closes (the MQTT listener died) it exits 2.
// Triggers that arrive while a video is playing are ignored. Logs started/finished lines.

import AppKit
import AVFoundation

setvbuf(stdout, nil, _IOLBF, 0)

let args = Array(CommandLine.arguments.dropFirst())
let playNow = args.contains("--now")
let videos = args.filter { !$0.hasPrefix("--") }.map { URL(fileURLWithPath: $0) }

func log(_ s: String) {
    let f = DateFormatter(); f.dateFormat = "HH:mm:ss"
    print("[\(f.string(from: Date()))] \(s)")
}

guard !videos.isEmpty else { fputs("No videos given.\n", stderr); exit(1) }
for v in videos where !FileManager.default.fileExists(atPath: v.path) {
    fputs("Video not found: \(v.path)\n", stderr); exit(1)
}

func isBuiltin(_ screen: NSScreen) -> Bool {
    guard let id = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? CGDirectDisplayID else { return false }
    return CGDisplayIsBuiltin(id) != 0
}

guard let screen = NSScreen.screens.first(where: { !isBuiltin($0) }) else {
    fputs("No external display found. Connect the projector first.\n", stderr)
    exit(1)
}

final class KeyWindow: NSWindow {
    override var canBecomeKey: Bool { true }
    override func keyDown(with event: NSEvent) {
        if event.keyCode == 53 { NSApp.terminate(nil) } // Esc
        else { super.keyDown(with: event) }
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)

let window = KeyWindow(contentRect: screen.frame, styleMask: [.borderless],
                       backing: .buffered, defer: false, screen: screen)
window.setFrame(screen.frame, display: true)
window.level = .screenSaver            // above the desktop, Dock and menu bar
window.backgroundColor = .black
window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]

let view = NSView(frame: NSRect(origin: .zero, size: screen.frame.size))
view.wantsLayer = true
view.layer = CALayer()
view.layer?.backgroundColor = NSColor.black.cgColor
window.contentView = view

var playing = false
var last: URL?

func play() {
    guard !playing else { log("trigger ignored, a video is already playing"); return }
    // Random pick, but not the same one twice in a row when there is a choice.
    let choices = videos.count > 1 ? videos.filter { $0 != last } : videos
    let url = choices.randomElement()!
    last = url
    playing = true

    let player = AVPlayer(url: url)
    let layer = AVPlayerLayer(player: player)
    layer.frame = view.bounds
    layer.videoGravity = .resizeAspectFill // cover, centered
    layer.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
    view.layer?.addSublayer(layer)

    var token: NSObjectProtocol?
    token = NotificationCenter.default.addObserver(forName: .AVPlayerItemDidPlayToEndTime,
                                                   object: player.currentItem, queue: .main) { _ in
        if let t = token { NotificationCenter.default.removeObserver(t) }
        layer.removeFromSuperlayer()   // back to plain black
        playing = false
        log("finished \(url.lastPathComponent)")
    }
    log("started \(url.lastPathComponent)")
    player.play()
}

// Each line on stdin is a trigger. EOF means the MQTT listener is gone.
let input = FileHandle.standardInput
var pending = Data()
input.readabilityHandler = { h in
    let data = h.availableData
    if data.isEmpty { DispatchQueue.main.async { exit(2) }; return }
    pending.append(data)
    while let nl = pending.firstIndex(of: 10) {
        pending.removeSubrange(pending.startIndex...nl)
        DispatchQueue.main.async { play() }
    }
}

NSCursor.hide()
window.makeKeyAndOrderFront(nil)
app.activate(ignoringOtherApps: true)
log("ready on external display (\(Int(screen.frame.width))x\(Int(screen.frame.height))), \(videos.count) video(s)")
if playNow { play() }
app.run()
