/*
  Wall photo + AS7341 color hex for the FireBeetle 2 ESP32-S3 (N16R8).

  The pins labeled SDA (GPIO1) and SCL (GPIO2) are wired to the camera
  connector, and the camera driver keeps that I2C controller. The color
  sensor uses the other controller, on two free pins:

    AS7341 +    ->  3V3
    AS7341 -    ->  GND
    AS7341 SCL  ->  SCK   (GPIO17)
    AS7341 SDA  ->  MISO  (GPIO16)

  Open http://wallcolor.local/ on the same 2.4 GHz Wi-Fi, or run pc/save_sample.py.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "DFRobot_AS7341.h"

#if __has_include("config.h")
#include "config.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

// Hardware V1.0 powers the camera through an AXP313A chip and needs the
// DFRobot_AXP313A library. V1.1 and later power the camera on their own.
#define BOARD_NEEDS_AXP313A 0

// A few V1.3 boards fail to stream JPEG. Set this true to capture RGB565
// and convert each frame before sending it.
constexpr bool CAMERA_FORCE_RGB565 = false;
// -1 keeps the camera default. Set 0 or 1 if the photo is upside down or mirrored.
constexpr int VFLIP = -1;
constexpr int HMIRROR = -1;

// Free GPIOs. GPIO1/GPIO2 are the camera connector.
constexpr int SENSOR_SDA = 16;  // silkscreen MISO
constexpr int SENSOR_SCL = 17;  // silkscreen SCK

// D14 is the onboard power button: holding it turns the board off.
// Both buttons are momentary, from the pin to GND.
// D3 and D6 each drive an LED: pin, 330 ohm, long leg, then GND.
// Both blink while a reading is in progress and stay on when it is done.
//   D12   read again
//   D11   store the current view as white
// The onboard LED follows D3 and D6.
constexpr int READ_BUTTON_PIN = 12;    // silkscreen D12, active low
constexpr int WHITE_BUTTON_PIN = 13;   // silkscreen D11, active low
constexpr int LED_A_PIN = 38;          // silkscreen D3, high = lit
constexpr int LED_B_PIN = 18;          // silkscreen D6, high = lit
constexpr uint8_t LED_OFF = 0;
constexpr uint8_t LED_BLINK = 1;
constexpr uint8_t LED_SOLID = 2;

constexpr uint8_t SENSOR_ATIME = 29;     // ~50 ms with ASTEP below
constexpr uint16_t SENSOR_ASTEP = 599;
constexpr uint8_t LED_LEVEL = 10;        // 1..20 -> about 4..42 mA
constexpr uint8_t GAIN_START = 5;        // 16x
constexpr uint8_t GAIN_MIN = 1;          // 1x
constexpr uint8_t GAIN_MAX = 9;          // 256x
constexpr uint16_t SATURATED_COUNT = 60000;
constexpr uint16_t LOW_CLEAR = 80;
constexpr uint32_t CAL_MAGIC = 0x434F4C31;  // "COL1"

constexpr int PWDN_GPIO_NUM = -1;
constexpr int RESET_GPIO_NUM = -1;
constexpr int XCLK_GPIO_NUM = 45;
constexpr int SIOD_GPIO_NUM = 1;
constexpr int SIOC_GPIO_NUM = 2;
constexpr int Y9_GPIO_NUM = 48;
constexpr int Y8_GPIO_NUM = 46;
constexpr int Y7_GPIO_NUM = 8;
constexpr int Y6_GPIO_NUM = 7;
constexpr int Y5_GPIO_NUM = 4;
constexpr int Y4_GPIO_NUM = 41;
constexpr int Y3_GPIO_NUM = 40;
constexpr int Y2_GPIO_NUM = 39;
constexpr int VSYNC_GPIO_NUM = 6;
constexpr int HREF_GPIO_NUM = 42;
constexpr int PCLK_GPIO_NUM = 5;

struct WhiteRef {
  uint32_t magic;
  float band[8];
  float clear;
};

struct ColorReading {
  bool saturated;
  uint8_t gain;
  uint16_t f[8];
  uint16_t clear;
  uint16_t nir;
  uint8_t r;
  uint8_t g;
  uint8_t b;
  const char *warning;
};

#if BOARD_NEEDS_AXP313A
#include <DFRobot_AXP313A.h>
DFRobot_AXP313A axp;
#endif

// Wire is I2C controller 0. The camera driver takes controller 1.
DFRobot_AS7341 as7341(&Wire);
WebServer server(80);
Preferences prefs;

bool sensorReady = false;
bool cameraReady = false;
bool wifiReady = false;
bool calibrated = false;
uint8_t sensorGain = GAIN_START;
WhiteRef whiteRef = {};

static const char INDEX_HTML[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Wall color</title>
<style>
  :root { color-scheme: light; }
  body { margin: 0; font: 16px/1.45 system-ui, sans-serif; background: #f4f1ea; color: #1c1915; }
  main { max-width: 760px; margin: 0 auto; padding: 28px 20px 48px; }
  h1 { font-size: 1.6rem; margin: 0 0 8px; }
  p { margin: 0 0 16px; }
  img { width: 100%; height: auto; background: #ddd6c8; display: block; }
  .photo { background: white; padding: 8px; border: 1px solid #e2dacb; }
  .color { display: flex; gap: 16px; align-items: center; margin: 16px 0; }
  .swatch { width: 96px; height: 96px; border: 1px solid #1c1915; background: #000; flex: none; }
  input[type="text"] { font: 1.6rem/1 ui-monospace, monospace; width: 9ch; border: 0; background: transparent; color: inherit; }
  .row { display: flex; flex-wrap: wrap; gap: 8px; margin: 8px 0 16px; }
  button { font: inherit; padding: 8px 14px; background: #1c1915; color: #f4f1ea; border: 0; cursor: pointer; }
  button.quiet { background: transparent; color: #1c1915; border: 1px solid #1c1915; }
  button:disabled { opacity: 0.5; cursor: wait; }
  #channels { font: 13px/1.6 ui-monospace, monospace; white-space: pre-wrap; }
  #status { min-height: 1.4em; color: #5c564c; }
</style>
</head>
<body>
<main>
  <h1>Wall color</h1>
  <p>Hold the color sensor a few centimetres from the wall, aimed at the spot you care about. The camera photographs whatever it is pointed at. Read the color first so the sensor LEDs are off before the photo.</p>
  <div class="photo"><img id="photo" alt="Latest wall photo"></div>
  <div class="color">
    <div id="swatch" class="swatch"></div>
    <div>
      <input id="hex" type="text" readonly value="#000000" aria-label="Color hex">
      <div id="meta"></div>
    </div>
  </div>
  <div class="row">
    <button id="again" type="button">Read again</button>
    <button id="copy" type="button" class="quiet">Copy hex</button>
    <button id="save" type="button" class="quiet">Save photo</button>
    <button id="white" type="button" class="quiet">This is white</button>
  </div>
  <div id="channels"></div>
  <p id="status">Starting…</p>
</main>
<script>
const photo = document.getElementById("photo");
const swatch = document.getElementById("swatch");
const hex = document.getElementById("hex");
const meta = document.getElementById("meta");
const channels = document.getElementById("channels");
const status = document.getElementById("status");
const buttons = [...document.querySelectorAll("button")];
let photoUrl = "";
let busy = false;

function setBusy(on) {
  busy = on;
  buttons.forEach((button) => { button.disabled = on; });
}

async function readJson(path) {
  const response = await fetch(path, { cache: "no-store" });
  const body = await response.json();
  if (!response.ok || body.ok === false) {
    throw new Error(body.error || ("Request failed (" + response.status + ")"));
  }
  return body;
}

function showColor(color) {
  hex.value = color.hex;
  swatch.style.background = color.hex;
  document.title = color.hex + " wall";
  const cal = color.calibrated ? "calibrated" : "not calibrated";
  meta.textContent = cal + (color.warning ? " · " + color.warning : "");
  const names = ["F1 415", "F2 445", "F3 480", "F4 515", "F5 555", "F6 590", "F7 630", "F8 680"];
  const lines = names.map((name, i) => name + "  " + color["f" + (i + 1)]);
  lines.push("Clear " + color.clear + "   NIR " + color.nir + "   gain " + color.gain);
  channels.textContent = lines.join("\n");
}

async function refresh() {
  if (busy) return;
  setBusy(true);
  try {
    status.textContent = "Reading color…";
    const color = await readJson("/color");
    showColor(color);
    status.textContent = "Taking photo…";
    await new Promise((resolve) => setTimeout(resolve, 250));
    const response = await fetch("/capture?t=" + Date.now(), { cache: "no-store" });
    if (!response.ok) throw new Error("Camera failed (" + response.status + ")");
    const blob = await response.blob();
    if (photoUrl) URL.revokeObjectURL(photoUrl);
    photoUrl = URL.createObjectURL(blob);
    photo.src = photoUrl;
    status.textContent = "Updated " + new Date().toLocaleTimeString();
  } catch (error) {
    status.textContent = error.message;
  } finally {
    setBusy(false);
  }
}

document.getElementById("again").onclick = refresh;
document.getElementById("copy").onclick = async () => {
  try {
    await navigator.clipboard.writeText(hex.value);
    status.textContent = "Copied " + hex.value;
  } catch (error) {
    hex.focus();
    hex.select();
    status.textContent = "Select the hex and copy it";
  }
};
document.getElementById("save").onclick = () => {
  if (!photoUrl) return;
  const link = document.createElement("a");
  link.href = photoUrl;
  link.download = "wall-" + hex.value.slice(1) + ".jpg";
  link.click();
};
document.getElementById("white").onclick = async () => {
  if (busy) return;
  setBusy(true);
  status.textContent = "Hold the sensor flat on a white card…";
  try {
    const color = await readJson("/calibrate");
    showColor(color);
    status.textContent = "White saved. The next readings use it.";
  } catch (error) {
    status.textContent = error.message;
  } finally {
    setBusy(false);
  }
};
refresh();
</script>
</body>
</html>
)html";

float gainMul(uint8_t gain) {
  static const float table[] = {0.5f, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
  if (gain > 10) gain = 10;
  return table[gain];
}

bool whiteRefSane(const WhiteRef &stored) {
  if (stored.magic != CAL_MAGIC || !(stored.clear > 0.0f)) return false;
  for (int i = 0; i < 8; i++) {
    if (!(stored.band[i] > 0.0f)) return false;
  }
  return true;
}

uint8_t clipByte(float value) {
  if (value < 0.0f) return 0;
  if (value > 255.0f) return 255;
  return (uint8_t)(value + 0.5f);
}

// Eight narrow bands do not map exactly onto sRGB. Each weight row sums to 1,
// so a flat spectrum (a white card after calibration) becomes #FFFFFF.
// The result is then scaled so the strongest channel is 255: the hex is the
// hue, and it stays put if the sensor moves a little closer or farther.
void spectrumToRgb(const float n[8], uint8_t &r, uint8_t &g, uint8_t &b) {
  const float rf = 0.20f * n[4] + 0.35f * n[5] + 0.30f * n[6] + 0.15f * n[7];
  const float gf = 0.15f * n[2] + 0.30f * n[3] + 0.30f * n[4] + 0.25f * n[5];
  const float bf = 0.45f * n[0] + 0.40f * n[1] + 0.15f * n[2];
  float peak = rf;
  if (gf > peak) peak = gf;
  if (bf > peak) peak = bf;
  if (peak < 0.0001f) {
    r = g = b = 0;
    return;
  }
  const float scale = 255.0f / peak;
  r = clipByte(rf * scale);
  g = clipByte(gf * scale);
  b = clipByte(bf * scale);
}

void paintReading(ColorReading &reading) {
  float sample[8];
  float peak = 0.0f;
  const float mul = gainMul(reading.gain);
  for (int i = 0; i < 8; i++) {
    sample[i] = reading.f[i] / mul;
    if (sample[i] > peak) peak = sample[i];
  }

  float n[8];
  if (calibrated) {
    for (int i = 0; i < 8; i++) n[i] = sample[i] / whiteRef.band[i];
  } else if (peak < 1.0f) {
    for (int i = 0; i < 8; i++) n[i] = 0.0f;
  } else {
    for (int i = 0; i < 8; i++) n[i] = sample[i] / peak;
  }
  spectrumToRgb(n, reading.r, reading.g, reading.b);
}

uint16_t measureOnce(ColorReading &reading) {
  as7341.setAGAIN(reading.gain);
  as7341.startMeasure(DFRobot_AS7341::eF1F4ClearNIR);
  DFRobot_AS7341::sModeOneData_t low = as7341.readSpectralDataOne();
  as7341.startMeasure(DFRobot_AS7341::eF5F8ClearNIR);
  DFRobot_AS7341::sModeTwoData_t high = as7341.readSpectralDataTwo();

  reading.f[0] = low.ADF1;
  reading.f[1] = low.ADF2;
  reading.f[2] = low.ADF3;
  reading.f[3] = low.ADF4;
  reading.f[4] = high.ADF5;
  reading.f[5] = high.ADF6;
  reading.f[6] = high.ADF7;
  reading.f[7] = high.ADF8;
  reading.clear = high.ADCLEAR;
  reading.nir = high.ADNIR;

  uint16_t peak = 0;
  for (int i = 0; i < 8; i++) {
    if (reading.f[i] > peak) peak = reading.f[i];
  }
  reading.saturated = peak >= SATURATED_COUNT;
  reading.warning = "";
  if (reading.saturated) reading.warning = "saturated — move the sensor a little farther from the wall";
  else if (reading.clear < LOW_CLEAR) reading.warning = "low light — move the sensor closer";
  return peak;
}

bool readColor(ColorReading &reading) {
  if (!sensorReady) return false;
  reading.gain = sensorGain;
  as7341.enableLed(true);
  delay(80);

  // One step toward a usable range, then a step back if that step saturates.
  uint16_t peak = measureOnce(reading);
  if (reading.saturated && reading.gain > GAIN_MIN) {
    reading.gain--;
    measureOnce(reading);
  } else if (peak < 400 && reading.gain < GAIN_MAX) {
    reading.gain++;
    peak = measureOnce(reading);
    if (reading.saturated && reading.gain > GAIN_MIN) {
      reading.gain--;
      measureOnce(reading);
    }
  }
  sensorGain = reading.gain;
  as7341.enableLed(false);
  paintReading(reading);
  return true;
}

int formatColorJson(const ColorReading &reading, char *buf, size_t len) {
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02X%02X%02X", reading.r, reading.g, reading.b);
  return snprintf(
    buf, len,
    "{\"ok\":true,\"hex\":\"%s\",\"r\":%u,\"g\":%u,\"b\":%u,"
    "\"f1\":%u,\"f2\":%u,\"f3\":%u,\"f4\":%u,\"f5\":%u,\"f6\":%u,\"f7\":%u,\"f8\":%u,"
    "\"clear\":%u,\"nir\":%u,\"gain\":%u,\"saturated\":%s,\"calibrated\":%s,\"warning\":\"%s\"}",
    hex, reading.r, reading.g, reading.b,
    reading.f[0], reading.f[1], reading.f[2], reading.f[3],
    reading.f[4], reading.f[5], reading.f[6], reading.f[7],
    reading.clear, reading.nir, reading.gain,
    reading.saturated ? "true" : "false",
    calibrated ? "true" : "false",
    reading.warning
  );
}

void sendJson(int code, const char *body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

void sendReading(const ColorReading &reading) {
  char body[768];
  const int written = formatColorJson(reading, body, sizeof(body));
  if (written <= 0 || written >= (int)sizeof(body)) {
    sendJson(500, "{\"ok\":false,\"error\":\"Could not format the reading\"}");
    return;
  }
  sendJson(200, body);
}

void handleIndex() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleColor() {
  beginActivity();
  ColorReading reading;
  if (!readColor(reading)) {
    sendJson(503, "{\"ok\":false,\"error\":\"Color sensor not found on MISO/SCK\"}");
    endActivity();
    return;
  }
  Serial.printf("Color %02X%02X%02X  clear=%u gain=%u\n", reading.r, reading.g, reading.b, reading.clear, reading.gain);
  sendReading(reading);
  endActivity();
}

void sendError(int code, const char *error) {
  char body[320];
  snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", error);
  sendJson(code, body);
}

// Saves a white-card reference and repaints the reading with it.
// On failure, error points at a message and the stored reference is left alone.
bool storeWhite(ColorReading &reading, const char *&error) {
  if (reading.saturated || reading.clear < LOW_CLEAR) {
    error = "White reading was too dark or too bright. Rest the sensor on the card and try again.";
    return false;
  }

  const float mul = gainMul(reading.gain);
  WhiteRef next = {};
  next.magic = CAL_MAGIC;
  next.clear = reading.clear / mul;
  bool usable = next.clear > 0.0f;
  for (int i = 0; i < 8; i++) {
    next.band[i] = reading.f[i] / mul;
    if (next.band[i] < 1.0f) usable = false;
  }
  if (!usable) {
    error = "The white card did not light every color channel. Move closer and try again.";
    return false;
  }

  whiteRef = next;
  prefs.putBytes("white", &whiteRef, sizeof(whiteRef));
  calibrated = true;
  paintReading(reading);
  reading.warning = "";
  error = nullptr;
  return true;
}

void handleCalibrate() {
  beginActivity();
  ColorReading reading;
  if (!readColor(reading)) {
    sendJson(503, "{\"ok\":false,\"error\":\"Color sensor not found on MISO/SCK\"}");
    endActivity();
    return;
  }
  const char *error = nullptr;
  if (!storeWhite(reading, error)) {
    sendError(400, error);
    endActivity();
    return;
  }
  Serial.printf("White saved. Sample reads %02X%02X%02X\n", reading.r, reading.g, reading.b);
  sendReading(reading);
  endActivity();
}

void handleCalibrateClear() {
  prefs.remove("white");
  calibrated = false;
  sendJson(200, "{\"ok\":true,\"calibrated\":false}");
}

void handleCapture() {
  if (!cameraReady) {
    server.send(503, "text/plain", "Camera failed to start");
    return;
  }

  camera_fb_t *frame = nullptr;
  for (int i = 0; i < 2; i++) {
    if (frame) esp_camera_fb_return(frame);
    frame = esp_camera_fb_get();
    if (!frame) break;
  }
  if (!frame) {
    server.send(503, "text/plain", "Camera did not return a frame");
    return;
  }

  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Content-Disposition", "inline; filename=wall.jpg");

  if (frame->format == PIXFORMAT_JPEG) {
    server.send_P(200, "image/jpeg", (const char *)frame->buf, frame->len);
    esp_camera_fb_return(frame);
    return;
  }

  uint8_t *jpeg = nullptr;
  size_t jpegLen = 0;
  const bool encoded = frame2jpg(frame, 80, &jpeg, &jpegLen);
  esp_camera_fb_return(frame);
  if (!encoded || jpeg == nullptr) {
    server.send(500, "text/plain", "Could not encode the photo");
    return;
  }
  server.send_P(200, "image/jpeg", (const char *)jpeg, jpegLen);
  free(jpeg);
}

void writeAll(const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const size_t chunk = (len - sent > 1024) ? 1024 : (len - sent);
    const size_t n = Serial.write(data + sent, chunk);
    if (n == 0) delay(2);
    else sent += n;
  }
  Serial.flush();
}

uint8_t *copyPhoto(size_t *outLen) {
  // Copy the picture out immediately. The camera reuses that memory, and a
  // picture still sitting there gets cut off while it is sent to the PC.
  *outLen = 0;
  uint8_t *best = nullptr;
  size_t bestLen = 0;
  int completeCount = 0;
  for (int i = 0; i < 8; i++) {
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) break;
    const bool complete = frame->format == PIXFORMAT_JPEG && frame->len > 4 &&
                          frame->buf[0] == 0xFF && frame->buf[1] == 0xD8 &&
                          frame->buf[frame->len - 2] == 0xFF && frame->buf[frame->len - 1] == 0xD9;
    if (complete) {
      uint8_t *copy = (uint8_t *)malloc(frame->len);
      if (copy) {
        memcpy(copy, frame->buf, frame->len);
        if (copy[frame->len - 2] == 0xFF && copy[frame->len - 1] == 0xD9) {
          free(best);
          best = copy;
          bestLen = frame->len;
          completeCount++;
        } else {
          free(copy);
        }
      }
    }
    esp_camera_fb_return(frame);
    if (completeCount >= 3) break;
  }
  *outLen = bestLen;
  return best;
}

void sendUsbColor(const char *body) {
  Serial.print("##COLOR ");
  Serial.println(body);
}

void fillColorBody(const ColorReading &reading, char *body, size_t len) {
  const int written = formatColorJson(reading, body, len);
  if (written <= 0 || written >= (int)len) {
    snprintf(body, len, "{\"ok\":false,\"hex\":\"#000000\",\"error\":\"Could not format the reading\"}");
  }
}

// Written only by the LED task, so a long camera capture still blinks.
volatile uint8_t ledPattern = LED_OFF;

void writeStatusLeds(bool on) {
  const uint8_t level = on ? HIGH : LOW;
  digitalWrite(LED_A_PIN, level);
  digitalWrite(LED_B_PIN, level);
  digitalWrite(LED_BUILTIN, level);
}

void ledTask(void *) {
  bool phase = false;
  for (;;) {
    const uint8_t pattern = ledPattern;
    if (pattern == LED_BLINK) {
      phase = !phase;
      writeStatusLeds(phase);
      vTaskDelay(pdMS_TO_TICKS(200));
    } else {
      writeStatusLeds(pattern == LED_SOLID);
      vTaskDelay(pdMS_TO_TICKS(40));
    }
  }
}

void beginActivity() {
  ledPattern = LED_BLINK;
}

void endActivity() {
  ledPattern = LED_SOLID;
}

struct EdgeButton {
  int pin;
  bool down;
  bool used;
  uint32_t changeMs;

  // One press starts one action. The pin has to go high again before the next one.
  bool consume() {
    const bool raw = digitalRead(pin) == LOW;
    const uint32_t now = millis();
    if (raw != down) {
      if (changeMs == 0) changeMs = now;
      else if (now - changeMs >= 25) {
        down = raw;
        changeMs = 0;
        if (!down) used = false;
      }
    } else {
      changeMs = 0;
    }
    if (down && !used) {
      used = true;
      return true;
    }
    return false;
  }
};

EdgeButton readButton = {READ_BUTTON_PIN, false, false, 0};
EdgeButton whiteButton = {WHITE_BUTTON_PIN, false, false, 0};

// PC sends "G" for a sample or "W" to store the current reading as white.
// D12 does the same as "G". D11 does the same as "W". Reply is one color
// line, then a JPEG (zero bytes for white), then ##END.
void sendUsbSample() {
  beginActivity();
  char body[768];
  ColorReading reading;
  if (!readColor(reading)) {
    snprintf(body, sizeof(body),
             "{\"ok\":false,\"hex\":\"#000000\",\"error\":\"AS7341 not found. SDA to MISO, SCL to SCK, + to 3V3.\"}");
  } else {
    fillColorBody(reading, body, sizeof(body));
  }

  sendUsbColor(body);

  // The color reading turns the sensor lights off. Turn them back on so the
  // close-up photo is not black, then off again before the packet ends.
  if (sensorReady) as7341.enableLed(true);
  size_t jpegLen = 0;
  uint8_t *jpeg = cameraReady ? copyPhoto(&jpegLen) : nullptr;
  if (sensorReady) as7341.enableLed(false);

  Serial.print("##JPEG ");
  Serial.println(jpegLen);
  if (jpeg && jpegLen) writeAll(jpeg, jpegLen);
  Serial.println();
  Serial.println("##END");

  free(jpeg);
  endActivity();
}

void sendUsbCalibrate() {
  beginActivity();
  char body[768];
  ColorReading reading;
  if (!readColor(reading)) {
    snprintf(body, sizeof(body),
             "{\"ok\":false,\"hex\":\"#000000\",\"error\":\"AS7341 not found. SDA to MISO, SCL to SCK, + to 3V3.\"}");
  } else {
    const char *error = nullptr;
    if (!storeWhite(reading, error)) {
      snprintf(body, sizeof(body), "{\"ok\":false,\"hex\":\"#000000\",\"error\":\"%s\"}", error);
    } else {
      Serial.printf("White saved. Sample reads %02X%02X%02X\n", reading.r, reading.g, reading.b);
      fillColorBody(reading, body, sizeof(body));
    }
  }

  sendUsbColor(body);
  Serial.println("##JPEG 0");
  Serial.println("##END");
  endActivity();
}

bool connectWifi() {
  if (strcmp(WIFI_SSID, "YOUR_WIFI_NAME") == 0) {
    Serial.println("Wi-Fi name is not set. USB preview is active.");
    return false;
  }

  Serial.printf("Connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname("wallcolor");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\nStill waiting for Wi-Fi. Check the name, the password, and that it is 2.4 GHz.");
      start = millis();
    }
  }
  Serial.println();
  Serial.print("http://");
  Serial.println(WiFi.localIP());
  return true;
}

bool startCamera() {
#if BOARD_NEEDS_AXP313A
  Wire.begin(1, 2);
  if (axp.begin() != 0) {
    Serial.println("AXP313A not found. Camera power was not enabled.");
  } else {
    axp.enableCameraPower(axp.eOV2640);
    Serial.println("Camera power enabled (V1.0 board).");
    delay(100);
  }
  Wire.end();
#endif

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  // One finished frame at a time. Two buffers let the camera overwrite a
  // picture while it is being copied, which cuts the photo off.
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_count = 1;

  if (CAMERA_FORCE_RGB565) {
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
  } else {
    config.pixel_format = PIXFORMAT_JPEG;
    // OV3660 tops out at 2048x1536. That frame only fits when PSRAM is on.
    config.frame_size = psramFound() ? FRAMESIZE_QXGA : FRAMESIZE_SVGA;
    config.jpeg_quality = 10;
  }

  if (!psramFound()) {
    Serial.println("PSRAM is off, so photos are 800x600.");
  } else {
    Serial.println("Photos are 2048x1536.");
  }

  const esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    Serial.println("Check the cable (pin 1 at the white dot), then power the board off and on.");
    Serial.println("A board marked V1.0 also needs BOARD_NEEDS_AXP313A set to 1.");
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    Serial.println("Camera started, but the sensor driver was missing.");
    return false;
  }
  if (sensor->id.PID == OV3660_PID) {
    sensor->set_vflip(sensor, 1);
    sensor->set_brightness(sensor, 1);
    sensor->set_saturation(sensor, -2);
    Serial.println("Camera: OV3660");
  } else if (sensor->id.PID == OV2640_PID) {
    Serial.println("Camera: OV2640");
  } else {
    Serial.printf("Camera sensor PID: 0x%04x\n", sensor->id.PID);
  }
  if (VFLIP >= 0) sensor->set_vflip(sensor, VFLIP);
  if (HMIRROR >= 0) sensor->set_hmirror(sensor, HMIRROR);
  sensor->set_whitebal(sensor, 1);
  sensor->set_awb_gain(sensor, 1);
  sensor->set_exposure_ctrl(sensor, 1);
  sensor->set_aec2(sensor, 1);
  sensor->set_gain_ctrl(sensor, 1);
  sensor->set_gainceiling(sensor, GAINCEILING_64X);
  return true;
}

bool startSensor() {
  // Pin the bus before the library calls Wire.begin() with no pin numbers.
  Wire.setPins(SENSOR_SDA, SENSOR_SCL);
  Wire.begin(SENSOR_SDA, SENSOR_SCL, 100000);
  if (as7341.begin() != 0) {
    Serial.println("AS7341 not found. + to 3V3, - to GND, SCL to SCK (GPIO17), SDA to MISO (GPIO16).");
    return false;
  }
  as7341.setAtime(SENSOR_ATIME);
  as7341.setAstep(SENSOR_ASTEP);
  as7341.setAGAIN(sensorGain);
  as7341.controlLed(LED_LEVEL);
  as7341.enableLed(false);
  Serial.printf("AS7341 ready, id 0x%02X\n", as7341.readID());
  return true;
}

void loadCalibration() {
  prefs.begin("wallcolor", false);
  WhiteRef stored = {};
  const size_t bytes = prefs.getBytes("white", &stored, sizeof(stored));
  if (bytes == sizeof(stored) && whiteRefSane(stored)) {
    whiteRef = stored;
    calibrated = true;
    Serial.println("White calibration loaded.");
  } else {
    Serial.println("No white calibration yet.");
  }
}

void setup() {
  pinMode(READ_BUTTON_PIN, INPUT_PULLUP);
  pinMode(WHITE_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_A_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  writeStatusLeds(false);
  xTaskCreatePinnedToCore(ledTask, "leds", 2048, nullptr, 1, nullptr, 0);

  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("Wall color camera");
  Serial.println("Read button: D12 to GND. White button: D11 to GND.");
  Serial.println("D3 and D6 blink while reading, then stay on.");

  loadCalibration();
  cameraReady = startCamera();
  sensorReady = startSensor();
  wifiReady = connectWifi();
  if (!wifiReady) {
    Serial.println("USB preview ready");
    return;
  }

  if (MDNS.begin("wallcolor")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("http://wallcolor.local/");
  }

  server.enableCORS(true);
  server.on("/", HTTP_GET, handleIndex);
  server.on("/color", HTTP_GET, handleColor);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/calibrate", HTTP_GET, handleCalibrate);
  server.on("/calibrate/clear", HTTP_GET, handleCalibrateClear);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  Serial.println("Ready.");
}

void loop() {
  if (readButton.consume()) sendUsbSample();
  else if (whiteButton.consume()) sendUsbCalibrate();
  if (!wifiReady) {
    while (Serial.available()) {
      const int c = Serial.read();
      if (c == 'G' || c == 'g') sendUsbSample();
      else if (c == 'W' || c == 'w') sendUsbCalibrate();
    }
    delay(10);
    return;
  }
  server.handleClient();
  delay(2);
}
