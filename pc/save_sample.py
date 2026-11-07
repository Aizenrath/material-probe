#!/usr/bin/env python3
"""Save a wall photo and the AS7341 color hex from the FireBeetle."""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent


def fetch(url, timeout):
    request = urllib.request.Request(url, headers={"Accept": "*/*"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def swatch(red, green, blue):
    if not sys.stdout.isatty():
        return ""
    return f"\033[48;2;{red};{green};{blue}m      \033[0m  "


def main():
    parser = argparse.ArgumentParser(
        description="Save a wall photo and print the color hex from the FireBeetle."
    )
    parser.add_argument(
        "host",
        nargs="?",
        default="wallcolor.local",
        help="Board address from the serial monitor (default: wallcolor.local)",
    )
    parser.add_argument(
        "--out",
        default=str(SCRIPT_DIR / "captures"),
        help="Folder for the JPEG and the color JSON",
    )
    parser.add_argument(
        "--calibrate",
        action="store_true",
        help="Store the current sensor reading as white, then exit",
    )
    args = parser.parse_args()

    base = f"http://{args.host}"
    try:
        if args.calibrate:
            body = fetch(base + "/calibrate", timeout=20)
            print(json.dumps(json.loads(body.decode("utf-8")), indent=2))
            return

        color_body = fetch(base + "/color", timeout=20)
        # The sensor LEDs switch off before this returns. Give the camera
        # a moment to expose the wall without that light.
        time.sleep(0.3)
        photo = fetch(base + "/capture", timeout=30)
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")
        print(f"The board answered {error.code}: {detail}", file=sys.stderr)
        sys.exit(1)
    except urllib.error.URLError as error:
        print(
            f"Could not reach {base}. Use the IP printed on the serial monitor, "
            f"and check that the PC is on the same 2.4 GHz Wi-Fi.\n{error.reason}",
            file=sys.stderr,
        )
        sys.exit(1)

    color = json.loads(color_body.decode("utf-8"))
    if not color.get("ok"):
        print(color.get("error", "Color reading failed"), file=sys.stderr)
        sys.exit(1)

    hex_color = color["hex"]
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    stem = f"wall-{stamp}-{hex_color[1:]}"
    folder = Path(args.out)
    folder.mkdir(parents=True, exist_ok=True)
    photo_path = folder / f"{stem}.jpg"
    color_path = folder / f"{stem}.json"
    photo_path.write_bytes(photo)
    color_path.write_text(json.dumps(color, indent=2) + "\n", encoding="utf-8")

    channels = "  ".join(f"F{i}={color[f'f{i}']}" for i in range(1, 9))
    print(f"{swatch(color['r'], color['g'], color['b'])}{hex_color}")
    print(f"Photo  {photo_path}")
    print(f"Color  {color_path}")
    print(channels)
    print(f"Clear={color['clear']}  NIR={color['nir']}  gain={color['gain']}")
    if color.get("warning"):
        print(f"Warning: {color['warning']}")
    if not color.get("calibrated"):
        print("Hold the sensor on a white card and run: python3 save_sample.py --calibrate")


if __name__ == "__main__":
    main()
