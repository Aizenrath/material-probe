# Wall photo and color

The FireBeetle 2 ESP32-S3 takes a still photo. The Gravity AS7341 reads the color of a small spot on the wall and turns it into a hex value. Both show up in a browser on the same Wi-Fi, and `pc/save_sample.py` saves the JPEG and the hex on the PC.

The sensor’s white LEDs stay on for the photo, so a close picture is not black. They switch off when the picture is done.

## What the customer does

The customer leaves the probe plugged into a Windows PC. They set it on the material and press the read button. Both LEDs blink while it works, then stay on when the photo and the hex are saved. Each press creates a **Material Probe** folder on the Desktop, if it is not there yet, and saves two files in it:

- `20261104-120501-C4A574.jpg` — the photo they hand to the AI
- `20261104-120501-C4A574.txt` — one line, the hex, such as `#C4A574`

The AI is their step. This device only produces the photo and the hex.

On their PC, once: copy `pc/MatProb.exe` and double-click it. Windows will not let the probe install a program by itself, so this one double-click is the setup. A window opens with the photo, the color, **Read again**, **This is white**, and **Open pictures**. After that the program starts whenever Windows starts. Plug in the probe and press the button. Windows 10 and 11 already include the driver. Nothing else is installed.

Before you hand it over:

1. Flash the board with the Wi-Fi name left as `YOUR_WIFI_NAME`, so it stays on USB.
2. Rest it on a white card and press the white button once. Both LEDs blink, then stay on. That reference stays on the board.
3. Double-click `MatProb.exe` on their PC.

## Wire it

The pins labeled **SDA** and **SCL** on the FireBeetle are the camera cable. The sensor’s SDA wire goes to the pin labeled **MISO**, and the sensor’s SCL wire goes to the pin labeled **SCK**.

| AS7341 label | Gravity cable | FireBeetle pin |
| --- | --- | --- |
| + | red | **3V3** |
| − | black | **GND** |
| SCL | blue | **SCK** (GPIO17) |
| SDA | green | **MISO** (GPIO16) |

Power the sensor from 3V3. The VCC pin is 5 V when the board is on USB, and that would put 5 V on the I2C lines.

Seat the camera cable with **pin 1 toward the white dot** next to the CAM socket, then close the latch. Pin 1 is marked on the cable.

The color sensor sits in the low snout, about 12 mm off the material. The camera is on the same face, higher up and 50 mm back, so the photo is the material above that spot. See `case/make_case.py` for the print and the fit.

Two momentary buttons, each wired from its pin to **GND**. Do not use the onboard **D14** button: holding that one turns the board off.

Two LEDs. Each one is the pin, a 330 Ω resistor, the long leg of the LED, and the short leg to **GND**. They always do the same thing: blink while a reading is in progress, then stay on when it has been sent to the PC. They are off until the first reading finishes. The onboard LED follows them.

| Added part | Connection | What it does |
| --- | --- | --- |
| Read button | **D12** and **GND** | Same as **Read again** |
| White button | **D11** and **GND** | Same as **This is white** |
| Status LED | **D3** → 330 Ω → LED → **GND** | Blinks while reading, then stays on |
| Status LED | **D6** → 330 Ω → LED → **GND** | Same as the LED on **D3** |

## Case

The customer is supplying their own case. `case/base.stl`, `case/lid.stl`, and `case/sensor_cap.stl` are an optional starting point. Regenerate them with `case/.venv/bin/python case/make_case.py` if you change a dimension.

The face that goes against the material:

- The low snout holds the AS7341, 12 mm off the surface. Drop it in from the top, window facing out, and clip `sensor_cap.stl` on.
- The camera lens is 50 mm back from that face, in the round hole above the snout. The photo is about 65 mm wide, of the material above the sensor. The snout covers the spot it is reading.
- Two small feet beside the snout keep the case from twisting.

The FireBeetle lies in the rails, USB out the back notch. Slide it forward if the camera cable is short. The lid takes a 12 mm button and a 5 mm LED, and four M3 screws close it. Print notes are at the top of `case/make_case.py`.

## Flash it

1. Install the Arduino IDE and the **ESP32** board package by Espressif (3.0 or newer).
2. Install the **DFRobot_AS7341** library from the Library Manager.
3. Open `firmware/WallColorCam/WallColorCam.ino`.
4. In `config.h`, set `WIFI_SSID` and `WIFI_PASSWORD`. The network has to be **2.4 GHz**. The ESP32-S3 cannot join 5 GHz.
5. Board: **DFRobot FireBeetle 2 ESP32-S3**.
   - Flash Mode: **QIO 80MHz**
   - USB CDC On Boot: **Enabled**
   - PSRAM: **OPI PSRAM**. Flash Mode stays **QIO 80MHz**. Photos are 2048×1536.
6. Upload. If the upload fails, hold **BOOT**, tap **RST**, release **BOOT**, and try again.

The serial monitor is **115200**. It prints an address such as `http://192.168.1.50/` and `http://wallcolor.local/`.

Boards silkscreened **V1.0** need the DFRobot AXP313A library and this line near the top of the sketch:

```cpp
#define BOARD_NEEDS_AXP313A 1
```

Leave it at `0` on V1.1 and later. If a photo never arrives on a board marked V1.3, set `CAMERA_FORCE_RGB565` to `true` and upload again.

## USB preview

With the placeholder Wi-Fi name still in `config.h`, the board sends each sample over the USB cable. On Windows, `pc/MatProb.exe` opens a window with the photo and the hex, and writes the Desktop folder.

On this Mac:

```bash
python3 pc/preview.py
```

A browser window opens with the latest photo, a color swatch, and the hex. **Read again** takes another one. Every photo is also written into a **Material Probe** folder on the Desktop, next to a text file that contains only the hex. **Open pictures** opens that folder. Rest the sensor on a white card and press the white button, or **This is white**, to save that reference on the board. The read button does the same thing as **Read again**. Both LEDs blink while that is happening, then stay on when the files are saved.

## Use it

On the PC, join the same Wi-Fi and open the address from the serial monitor.

**Read again** takes a new color reading and a new photo. **Copy hex** copies `#RRGGBB`. **Save photo** downloads the JPEG. The filename includes the hex.

Or from this folder:

```bash
python3 pc/save_sample.py
```

If `wallcolor.local` does not resolve, pass the IP from the serial monitor:

```bash
python3 pc/save_sample.py 192.168.1.50
```

Each run writes a pair of files in `pc/captures/`:

- `wall-YYYYMMDD-HHMMSS-C4A574.jpg`
- `wall-YYYYMMDD-HHMMSS-C4A574.json`

The same reading is available as JSON at `http://wallcolor.local/color` and as a JPEG at `http://wallcolor.local/capture`.

## White card

The AS7341 measures eight slices of the spectrum, plus a clear channel and near-infrared. The sketch turns those slices into a hex. It is a close description of the hue, and a white card makes it accurate.

Rest the sensor flat on a white card, with the LEDs facing the card, and either press the white button, press **This is white**, or run:

```bash
python3 pc/save_sample.py --calibrate
```

That reference stays on the board. A later white reading should land on `#FFFFFF`. To forget it, open `http://wallcolor.local/calibrate/clear`.

The hex follows hue. A darker red and a brighter red can share it if the mix of wavelengths is the same. The `clear` number in the JSON is how much light came back.

## If something fails

**Wi-Fi never connects.** The name or password in `config.h` is wrong, or the network is 5 GHz. The serial monitor prints a dot every half second until it joins.

**AS7341 not found.** Check +, −, SCL→SCK, SDA→MISO, and that the sensor is on 3V3. The camera still serves photos.

**Camera init failed.** Reseat the cable with pin 1 at the white dot, then unplug USB and plug it back in. Confirm PSRAM is **OPI PSRAM**. On a V1.0 board, set `BOARD_NEEDS_AXP313A` to `1`.

**The picture is upside down or mirrored.** At the top of the sketch, set `VFLIP` or `HMIRROR` to `0` or `1`.

**The hex looks wrong.** Calibrate on a white card. Keep the sensor a few centimetres from the wall so the warning is neither “low light” nor “saturated”. The LEDs have to see the wall. Room light alone is a weaker, less repeatable reading.
