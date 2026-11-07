#!/usr/bin/env python3
"""Open a window that shows the FireBeetle photo and the AS7341 hex.

The board has to be on USB. With no Wi-Fi name in config.h it answers
"G" for a sample and "W" to store a white card. This script shows both at
http://127.0.0.1:8765/
"""

import json
import os
import subprocess
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial
import serial.tools.list_ports

HOST = "127.0.0.1"
PORT = 8765
PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Material probe</title>
<style>
  body { margin: 0; font: 16px/1.4 system-ui, sans-serif; background: #f3efe6; color: #1c1915; }
  main { max-width: 920px; margin: 0 auto; padding: 24px 20px 40px; }
  h1 { font-size: 1.4rem; margin: 0 0 12px; }
  img { width: 100%; height: auto; background: #d9d3c7; display: block; }
  .frame { background: white; padding: 8px; border: 1px solid #e0d8c8; }
  .readout { display: flex; gap: 16px; align-items: center; margin: 16px 0; }
  .swatch { width: 112px; height: 112px; border: 1px solid #1c1915; background: #000; flex: none; }
  #hex { font: 2rem/1 ui-monospace, monospace; margin: 0; }
  #detail { color: #5c564c; min-height: 1.4em; }
  .actions { display: flex; gap: 8px; flex-wrap: wrap; }
  button { font: inherit; padding: 8px 14px; background: #1c1915; color: #f3efe6; border: 1px solid #1c1915; cursor: pointer; }
  button.ghost { background: transparent; color: #1c1915; }
  button:disabled { opacity: 0.45; cursor: default; }
  #channels { font: 13px/1.6 ui-monospace, monospace; white-space: pre-wrap; }
  #where { color: #5c564c; font-size: 14px; word-break: break-all; }
</style>
</head>
<body>
<main>
  <h1>Material probe</h1>
  <div class="frame"><img id="photo" alt="Latest photo from the FireBeetle"></div>
  <div class="readout">
    <div id="swatch" class="swatch"></div>
    <div>
      <p id="hex">waiting</p>
      <p id="detail">Asking the board for a photo…</p>
    </div>
  </div>
  <div class="actions">
    <button id="again" type="button">Read again</button>
    <button id="white" class="ghost" type="button">This is white</button>
    <button id="folder" class="ghost" type="button">Open pictures</button>
  </div>
  <p id="where">Each reading is saved in this folder: <span id="folderpath"></span></p>
  <div id="channels"></div>
</main>
<script>
const photo = document.getElementById("photo");
const swatch = document.getElementById("swatch");
const hex = document.getElementById("hex");
const detail = document.getElementById("detail");
const channels = document.getElementById("channels");
const buttons = [...document.querySelectorAll("button")];
document.getElementById("folderpath").textContent = __FOLDER_JSON__;
let busy = false;
let seen = 0;
let offline = false;
let started = false;

function show(color, note) {
  if (!color.ok) {
    detail.textContent = color.error || "No color reading";
    return;
  }
  const value = color.hex || "#000000";
  hex.textContent = value;
  swatch.style.background = value;
  document.title = value + " material probe";
  const bits = [];
  if (note) bits.push(note);
  if (color.file) bits.push("Saved " + color.file);
  else bits.push(color.calibrated ? "Calibrated" : "Not calibrated");
  if (color.warning) bits.push(color.warning);
  detail.textContent = bits.join(" · ");
  const names = ["F1 415", "F2 445", "F3 480", "F4 515", "F5 555", "F6 590", "F7 630", "F8 680"];
  channels.textContent = names.map((name, i) => name + "  " + color["f" + (i + 1)]).join("\\n")
    + "\\nClear " + color.clear + "   NIR " + color.nir;
  if (color.has_photo) photo.src = "/photo.jpg?t=" + Date.now();
}

async function ask(path, waiting, note) {
  if (busy) return;
  busy = true;
  buttons.forEach((button) => { button.disabled = true; });
  detail.textContent = waiting;
  try {
    const response = await fetch(path, { cache: "no-store" });
    const color = await response.json();
    if (color.seq) seen = color.seq;
    show(color, color.ok ? note : "");
  } catch (error) {
    detail.textContent = error.message;
  } finally {
    busy = false;
    buttons.forEach((button) => { button.disabled = false; });
  }
}

async function poll() {
  try {
    const response = await fetch("/state", { cache: "no-store" });
    const color = await response.json();
    if (color.connected === false) {
      offline = true;
      started = false;
      detail.textContent = "Plug the probe into a USB port.";
      return;
    }
    if (offline) {
      offline = false;
      detail.textContent = "Board connected.";
    }
    if (color.seq && color.seq !== seen) {
      seen = color.seq;
      started = true;
      show(color, "");
      return;
    }
    if (!started) {
      started = true;
      ask("/sample", "Asking the board for a photo…", "");
    }
  } catch (error) {
  }
}

document.getElementById("again").onclick = () => ask("/sample", "Reading the sensor and taking a photo…", "");
document.getElementById("white").onclick = () => ask("/calibrate", "Hold the sensor flat on a white card…", "White saved");
document.getElementById("folder").onclick = () => fetch("/folder", { cache: "no-store" });
setInterval(poll, 400);
poll();
</script>
</body>
</html>
"""

board = None
link = None
state = threading.Condition()
pending = None
sample_seq = 0
latest_jpeg = b""
latest_color = {
    "ok": False,
    "hex": "#000000",
    "error": "No sample yet",
    "has_photo": False,
    "seq": 0,
    "connected": False,
}

# This firmware uses the ESP32-S3 USB-Serial/JTAG port. Windows 10 and 11
# bind that to a COM port with the built-in USB serial driver.
BOARD_IDS = {(0x303A, 0x1001)}
LOG = Path(__file__).resolve().parent / "probe.log"


def log(message):
    print(message, flush=True)
    try:
        with LOG.open("a", encoding="utf-8") as handle:
            handle.write(time.strftime("%Y-%m-%d %H:%M:%S ") + message + "\n")
    except OSError:
        pass


def matching_ports():
    found = []
    for port in serial.tools.list_ports.comports():
        if (port.vid, port.pid) in BOARD_IDS:
            found.append(port.device)
    return sorted(found)


def find_port():
    announced = False
    while True:
        ports = matching_ports()
        if ports:
            return ports[-1]
        if not announced:
            log("Plug in the FireBeetle.")
            announced = True
        time.sleep(0.4)


class Link:
    """Bytes already pulled off the USB port, so a button press is not dropped."""

    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()

    def read_line(self, deadline):
        while time.time() < deadline:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line = self.buf[:nl].decode("utf-8", "replace").rstrip("\r")
                del self.buf[: nl + 1]
                return line
            chunk = self.ser.read(max(1, min(self.ser.in_waiting, 4096)))
            if chunk:
                self.buf += chunk
        return None

    def read_exact(self, size, deadline):
        while len(self.buf) < size:
            if time.time() > deadline:
                raise TimeoutError(f"Photo stopped at {len(self.buf)} of {size} bytes.")
            chunk = self.ser.read(size - len(self.buf))
            if chunk:
                self.buf += chunk
        out = bytes(self.buf[:size])
        del self.buf[:size]
        return out


def wait_ready(ser):
    deadline = time.time() + 20
    buf = b""
    while time.time() < deadline:
        incoming = ser.read(256)
        if not incoming:
            continue
        buf = (buf + incoming)[-400:]
        if b"USB preview ready" in buf:
            return
    raise TimeoutError("The board did not start the USB preview. Flash WallColorCam again.")


def open_board():
    path = find_port()
    ser = serial.Serial()
    ser.port = path
    ser.baudrate = 115200
    ser.timeout = 0.3
    ser.write_timeout = 3
    # Leave both lines low before open so Windows does not reset into the bootloader.
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = True
    time.sleep(0.12)
    ser.rts = False
    time.sleep(0.4)
    wait_ready(ser)
    log(f"Board ready on {path}")
    return ser


def desktop_folder():
    if sys.platform == "win32":
        import ctypes
        buffer = ctypes.create_unicode_buffer(260)
        # The real Desktop directory, including when OneDrive has moved it.
        if ctypes.windll.shell32.SHGetFolderPathW(None, 0x0010, None, 0, buffer) == 0 and buffer.value:
            return Path(buffer.value)
    return Path.home() / "Desktop"


CAPTURES = desktop_folder() / "Material Probe"


def save_reading(color, photo):
    if not photo:
        return
    CAPTURES.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    hexpart = (color.get("hex") or "000000").lstrip("#")
    stem = f"{stamp}-{hexpart}"
    jpg = CAPTURES / f"{stem}.jpg"
    n = 2
    while jpg.exists():
        jpg = CAPTURES / f"{stem}-{n}.jpg"
        n += 1
    jpg.write_bytes(photo)
    jpg.with_suffix(".txt").write_text((color.get("hex") or "") + "\n", encoding="utf-8")
    color["file"] = jpg.name
    print(f"Saved {jpg}")


def publish(color, photo):
    global latest_jpeg, latest_color, sample_seq
    save_reading(color, photo)
    with state:
        if photo:
            latest_jpeg = photo
        color["has_photo"] = bool(photo)
        color["connected"] = True
        sample_seq += 1
        color["seq"] = sample_seq
        latest_color = color
        state.notify_all()


def open_captures():
    CAPTURES.mkdir(parents=True, exist_ok=True)
    if sys.platform == "darwin":
        subprocess.Popen(["open", str(CAPTURES)])
    elif sys.platform == "win32":
        os.startfile(str(CAPTURES))
    else:
        subprocess.Popen(["xdg-open", str(CAPTURES)])


def mark_connected(connected):
    global latest_color
    with state:
        latest_color = dict(latest_color)
        latest_color["connected"] = connected
        state.notify_all()


def close_board():
    global board
    try:
        if board is not None:
            board.close()
    except Exception:
        pass
    board = None


def reconnect():
    global board, link
    close_board()
    mark_connected(False)
    print("Board unplugged. Waiting for it to come back.")
    while True:
        try:
            board = open_board()
            link = Link(board)
            link.buf.clear()
            mark_connected(True)
            return
        except Exception as error:
            print(f"Waiting for the board: {error}", file=sys.stderr)
            close_board()
            time.sleep(0.8)


def send_pending():
    global pending
    with state:
        command = pending
        pending = None
    if command:
        board.write(command)
        board.flush()


def read_sample(deadline):
    color_line = ""
    while time.time() < deadline:
        line = link.read_line(deadline)
        if line and line.startswith("##COLOR "):
            color_line = line[len("##COLOR ") :]
            break
    if not color_line:
        return None
    deadline = time.time() + 20
    color = json.loads(color_line)
    jpeg_len = None
    while time.time() < deadline:
        line = link.read_line(deadline)
        if line and line.startswith("##JPEG "):
            jpeg_len = int(line.split()[1])
            break
    photo = link.read_exact(jpeg_len, deadline) if jpeg_len else b""
    while time.time() < deadline:
        if link.read_line(deadline) == "##END":
            break
    return color, photo


def listen():
    while True:
        try:
            send_pending()
            found = read_sample(time.time() + 0.3)
            if found:
                publish(*found)
        except Exception as error:
            print(f"Serial: {error}", file=sys.stderr)
            reconnect()


def request(command):
    global pending
    with state:
        start = sample_seq
        pending = command
        deadline = time.time() + 40
        while sample_seq == start:
            remaining = deadline - time.time()
            if remaining <= 0:
                if pending == command:
                    pending = None
                raise TimeoutError("The board is not answering. Plug it in and press Read again.")
            state.wait(timeout=min(remaining, 0.5))
        return dict(latest_color)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split("?", 1)[0]
        try:
            if path == "/":
                body = PAGE.replace("__FOLDER_JSON__", json.dumps(str(CAPTURES))).encode("utf-8")
                self._send(200, "text/html; charset=utf-8", body)
            elif path == "/state":
                with state:
                    body = json.dumps(latest_color).encode("utf-8")
                self._send(200, "application/json", body)
            elif path == "/sample":
                color = request(b"G")
                body = json.dumps(color).encode("utf-8")
                self._send(200, "application/json", body)
            elif path == "/calibrate":
                color = request(b"W")
                body = json.dumps(color).encode("utf-8")
                self._send(200, "application/json", body)
            elif path == "/folder":
                open_captures()
                self._send(200, "text/plain", b"ok")
            elif path == "/photo.jpg":
                with state:
                    photo = latest_jpeg
                if not photo:
                    self._send(404, "text/plain", b"No photo yet")
                    return
                self._send(200, "image/jpeg", photo)
            else:
                self._send(404, "text/plain", b"Not found")
        except Exception as error:
            body = json.dumps({"ok": False, "hex": "#000000", "error": str(error), "has_photo": False}).encode()
            self._send(500, "application/json", body)

    def _send(self, code, content_type, body):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        return


def board_loop():
    global board, link
    while True:
        try:
            board = open_board()
            link = Link(board)
            mark_connected(True)
            listen()
        except Exception as error:
            log(f"Waiting for the board: {error}")
            close_board()
            mark_connected(False)
            time.sleep(0.8)


def main():
    url = f"http://{HOST}:{PORT}/"
    try:
        if LOG.exists() and LOG.stat().st_size > 200_000:
            LOG.write_text("", encoding="utf-8")
    except OSError:
        pass
    try:
        server = ThreadingHTTPServer((HOST, PORT), Handler)
    except OSError:
        log(f"Already running. Opening {url}")
        webbrowser.open(url)
        return
    log(f"Pictures folder: {CAPTURES}")
    log(f"Preview window: {url}")
    threading.Thread(target=board_loop, daemon=True).start()
    threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        close_board()


if __name__ == "__main__":
    main()
