#!/usr/bin/env python3
"""Runs on santa (Raspberry Pi) and drives mpv on the projector.

Plays loop-30s.mp4 on repeat. Each line on stdin (an elf/intro/end MQTT message, fed by
projector-santa.sh) switches to a random santa video: the loop fades out (150 ms), the santa
video fades in (150 ms), and at its end it fades out again and the loop fades in from black.
Fades are done by stepping mpv's brightness (and volume) over its IPC socket.

Usage: projector-santa.py [--now] video.mp4 [video2.mp4 ...]   (the loop is loop-30s.mp4 next to this file)
Click the screen while the loop plays (or press Esc) to quit (exit 0). Exits 2 if stdin closes
(the MQTT listener died), 1 on errors. Triggers during a santa video or a transition are ignored.
"""
import json, os, random, socket, subprocess, sys, tempfile, threading, time

OUT, IN = 0.15, 0.15      # loop fades out / santa fades in at the start of a santa video
SANTA_OUT = 0.15          # santa fades out at its end
LOOP_IN = 1.5             # loop fades in from black after a santa video
STEP = 0.02

here = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[1:]
play_now = "--now" in args
videos = [a for a in args if not a.startswith("--")]
loop = os.path.join(here, "loop-30s.mp4")

def log(msg): print(time.strftime("[%H:%M:%S] ") + msg, flush=True)

if not videos: sys.exit("No videos given.")
for v in videos + [loop]:
    if not os.path.exists(v): sys.exit(f"Video not found: {v}")

tmp = tempfile.mkdtemp(prefix="elf-mpv.")
sock_path = os.path.join(tmp, "mpv.sock")
conf = os.path.join(tmp, "input.conf")
with open(conf, "w") as f:
    f.write("MBTN_LEFT script-message click\nESC script-message quit\n")

mpv = subprocess.Popen([
    "mpv", "--no-terminal", "--idle=yes", "--force-window=yes", "--fullscreen", "--keep-open=no",
    "--panscan=1.0", "--background-color=#000000", "--osd-level=0", "--no-osc",
    "--cursor-autohide=1000", "--input-default-bindings=no", f"--input-conf={conf}",
    f"--input-ipc-server={sock_path}", "--brightness=-100", "--volume=100"])

for _ in range(100):
    if os.path.exists(sock_path): break
    time.sleep(0.1)
else:
    sys.exit("mpv did not start")
sock = socket.socket(socket.AF_UNIX); sock.connect(sock_path)

lock = threading.Lock()            # state
state = "loop-start"               # loop | santa | busy
file_loaded = threading.Event()    # playback of the newest file has (re)started
ended = threading.Event()
fading_out = threading.Event()
req = [0]

def send(*command):
    req[0] += 1
    sock.sendall((json.dumps({"command": list(command), "request_id": req[0]}) + "\n").encode())

def shutdown(code):
    try: send("quit")
    except OSError: pass
    time.sleep(0.2); mpv.terminate()
    os._exit(code)

def reader():
    buf = b""
    while True:
        data = sock.recv(65536)
        if not data: shutdown(1)
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            try: ev = json.loads(line)
            except ValueError: continue
            name = ev.get("event")
            if name == "playback-restart": file_loaded.set()
            elif name == "property-change" and ev.get("name") == "time-remaining":
                left = ev.get("data")
                if state == "santa" and left is not None and left <= SANTA_OUT and not fading_out.is_set():
                    fading_out.set()
                    threading.Thread(target=fade, args=(0, -100, max(left, 0.05)), daemon=True).start()
            elif name == "end-file" and ev.get("reason") in ("eof", "error"): ended.set()
            elif name == "client-message":
                a = ev.get("args", [])
                if a == ["quit"] or (a == ["click"] and state == "loop"):
                    log("quit requested"); shutdown(0)

def fade(start, end, secs):        # brightness and volume together, -100..0 <-> 0..100
    t0 = time.time()
    while True:
        k = min(1.0, (time.time() - t0) / secs)
        b = start + (end - start) * k
        send("set_property", "brightness", round(b))
        send("set_property", "volume", round(100 + b))
        if k >= 1.0: return
        time.sleep(STEP)

def load(path, repeat=False):
    file_loaded.clear(); ended.clear()
    send("set_property", "loop-file", "inf" if repeat else "no")
    send("loadfile", path, "replace")
    file_loaded.wait(10)

def start_loop(secs):
    global state
    state = "busy"
    load(loop, repeat=True)
    fade(-100, 0, secs)
    state = "loop"
    log("loop playing")

last = None
def play_santa():
    global state, last
    with lock:
        if state != "loop":
            log("trigger ignored, busy"); return
        state = "busy"
    choices = [v for v in videos if v != last] or videos
    last = random.choice(choices)
    name = os.path.basename(last)
    log(f"started {name}")
    fade(0, -100, OUT)
    load(last)
    fade(-100, 0, IN)
    state = "santa"
    # The reader fades it out just before the end; then bring the loop back.
    fading_out.clear()
    ended.wait(timeout=None)
    log(f"finished {name}")
    start_loop(LOOP_IN)

threading.Thread(target=reader, daemon=True).start()
send("observe_property", 1, "time-remaining")
start_loop(LOOP_IN)
log(f"ready, {len(videos)} video(s)")
if play_now: threading.Thread(target=play_santa, daemon=True).start()

for line in sys.stdin:
    threading.Thread(target=play_santa, daemon=True).start()
log("stdin closed")
shutdown(2)
