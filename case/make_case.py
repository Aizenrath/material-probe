#!/usr/bin/env python3
"""Printable case: sensor low on the material, camera higher and set back.

The face that touches the material has two openings, one above the other.

  Color sensor   A snout at the bottom. Feet keep the AS7341 12 mm off the
                 material. Drop the module in from above, sensor facing the
                 window, then fit sensor_cap.stl.
  Camera         The lens is 50 mm back from that same face, centered above
                 the snout. At the lens's 68 degree view that covers about
                 65 mm x 48 mm of the material directly above the sensor.
                 The snout hides the spot it is touching; the photo is the
                 material around that spot.

Focus the lens for 5 cm before the lid goes on. It ships focused for about
half a metre. If the camera cable is short, slide the FireBeetle forward
in the rails until the cable reaches the pocket behind the lens hole.

Print in PLA, 0.2 mm layers, 3 perimeters, 15 percent infill:
  base.stl         feet and snout on the bed, open side up
  lid.stl          plate on the bed, lip upward; flip it to close the body
  sensor_cap.stl   plate on the bed, lip upward; flip it onto the snout

Close the body with four M3 x 8 mm self-tapping screws into the 2.6 mm
pilots (lid holes are 3.4 mm). This lid has one 12 mm button hole and
one 5 mm LED hole. The sketch also expects a second button and two
more LEDs, so those holes have to be added before printing:

  D12 to GND     read button
  D11 to GND     white button
  D3, 330 ohm    status LED, blinks while reading, then stays on
  D6, 330 ohm    status LED, same pattern as D3

USB-C leaves the notch at the back. Do not use the onboard D14 button.

Regenerate the STLs with:  case/.venv/bin/python case/make_case.py
"""

import struct
from pathlib import Path

from manifold3d import Manifold

HERE = Path(__file__).resolve().parent

# x = 0 is the face that touches the material. x grows away from it.
# y is width. z is up. The snout is the low front. The camera is above it.
WALL = 2.4
LENS_X = 50.0
LENS_Y = 36.0
LENS_Z = 47.0
BODY_X0 = 20.0
BODY_X1 = 128.0
BODY_Y1 = 72.0
BODY_Z1 = 64.0
SNOUT_X1 = 36.0
SNOUT_Y0 = 18.0
SNOUT_Y1 = 54.0
SNOUT_Z1 = 28.0
POSTS = ((66.0, 8.0), (66.0, 64.0), (114.0, 8.0), (114.0, 64.0))


def box(x, y, z, dx, dy, dz):
    return Manifold.cube((dx, dy, dz)).translate((x, y, z))


def cyl(x, y, z, height, radius, segments=48):
    return Manifold.cylinder(height, radius, radius, segments).translate((x, y, z))


def cyl_x(x, y, z, length, radius, segments=48):
    # Cylinder is built along Z. Turn it so it runs toward +x, centered on y and z.
    return Manifold.cylinder(length, radius, radius, segments).rotate((0, 90, 0)).translate((x, y, z))


def solid_at(part, x, y, z):
    probe = box(x - 0.4, y - 0.4, z - 0.4, 0.8, 0.8, 0.8)
    return (part ^ probe).volume() > 0


def write_stl(path, part):
    mesh = part.to_mesh()
    verts = mesh.vert_properties
    tris = mesh.tri_verts
    with path.open("wb") as out:
        out.write(b"material probe".ljust(80, b"\0"))
        out.write(struct.pack("<I", len(tris)))
        for a, b, c in tris:
            p0, p1, p2 = (tuple(float(v) for v in verts[i][:3]) for i in (a, b, c))
            ux, uy, uz = (p1[i] - p0[i] for i in range(3))
            vx, vy, vz = (p2[i] - p0[i] for i in range(3))
            nx = uy * vz - uz * vy
            ny = uz * vx - ux * vz
            nz = ux * vy - uy * vx
            length = (nx * nx + ny * ny + nz * nz) ** 0.5 or 1.0
            out.write(struct.pack(
                "<12fH",
                nx / length, ny / length, nz / length,
                *p0, *p1, *p2, 0,
            ))
    print(f"Wrote {path.name}  ({path.stat().st_size / 1024:.0f} KB)")


def base():
    body = box(BODY_X0, 0, 0, BODY_X1 - BODY_X0, BODY_Y1, BODY_Z1)
    # The view in front of the lens stays open. The snout is below this cut.
    view = box(-1, 6, 30, LENS_X - 1, 60, 50)
    cavity = box(LENS_X, WALL, WALL, BODY_X1 - LENS_X - WALL, BODY_Y1 - 2 * WALL, BODY_Z1)
    part = body - view - cavity

    snout = box(0, SNOUT_Y0, 0, SNOUT_X1, SNOUT_Y1 - SNOUT_Y0, SNOUT_Z1)
    # 12 mm of open frame, then a pocket the module drops into from above.
    frame = box(-1, 22, 2, 13, 28, 24)
    pocket = box(12, 20, 1.2, 22, 32, SNOUT_Z1)
    snout = snout - frame - pocket
    # Wires run back along the floor into the main cavity.
    channel = box(30, 30, 2, 24, 12, 8)

    feet = box(0, 0, 0, 16, 14, 10) + box(0, 58, 0, 16, 14, 10)

    plate = box(LENS_X - 2.4, 16, 32, 2.4, 40, 28)
    lens = cyl_x(LENS_X - 4, LENS_Y, LENS_Z, 10, 5.5, 64)

    # Rails for the 25.4 mm wide FireBeetle. It can slide toward the camera.
    rail_z = WALL
    rails = box(62, 20.6, rail_z, 56, 2.0, 2.6) + box(62, 49.4, rail_z, 56, 2.0, 2.6)
    stop = box(60, 22.6, rail_z, 2.0, 26.8, 2.6)

    posts = Manifold()
    for px, py in POSTS:
        posts += cyl(px, py, WALL, BODY_Z1 - WALL, 3.8, 32)
        posts -= cyl(px, py, BODY_Z1 - 10, 12, 1.3, 24)

    usb = box(BODY_X1 - WALL - 1, 29, 6, WALL + 4, 14, BODY_Z1)

    part = part + snout + feet + plate + rails + stop + posts
    # The body is solid where the snout overlaps it, so cut the pocket again after the union.
    part = part - channel - lens - usb - frame - pocket
    return part


def lid():
    # Covers the body behind the lens. Printed lip-up, then flipped on.
    x0 = LENS_X + 2
    plate = box(x0, 0, 0, BODY_X1 - x0, BODY_Y1, 2.6)
    inset = WALL + 0.4
    lip = box(x0 + inset, inset, 2.6, BODY_X1 - x0 - 2 * inset, BODY_Y1 - 2 * inset, 4)
    inner = box(
        x0 + inset + 1.6, inset + 1.6, 2.6,
        BODY_X1 - x0 - 2 * (inset + 1.6), BODY_Y1 - 2 * (inset + 1.6), 5,
    )
    part = plate + lip - inner
    part -= cyl(92, LENS_Y, -1, 5, 6.1, 48)  # 12 mm button
    part -= cyl(92, 54, -1, 5, 2.6, 32)  # 5 mm LED
    for px, py in POSTS:
        part -= cyl(px, py, -1, 5, 1.7, 24)
    return part


def sensor_cap():
    # The lip drops into the open pocket (x 12–34, y 20–52), not into the front frame.
    plate = box(0, SNOUT_Y0, 0, SNOUT_X1, SNOUT_Y1 - SNOUT_Y0, 2.2)
    lip = box(14, 22, 2.2, 18, 28, 3.0)
    inner = box(15.6, 23.6, 2.2, 14.8, 24.8, 4)
    return plate + lip - inner


def main():
    parts = {"base.stl": base(), "lid.stl": lid(), "sensor_cap.stl": sensor_cap()}
    body = parts["base.stl"]
    checks = {
        "lens hole": ((LENS_X, LENS_Y, LENS_Z), False),
        "sensor window": ((6, 36, 14), False),
        "view above the snout": ((20, LENS_Y, 55), False),
        "board cavity": ((90, LENS_Y, 30), False),
        "snout wall": ((6, SNOUT_Y0 + 1.2, 14), True),
        "side foot": ((8, 6, 4), True),
        "lens plate": ((LENS_X - 1.2, LENS_Y, LENS_Z + 9), True),
        "sensor pocket": ((22, 36, 12), False),
        "usb notch": ((BODY_X1 - 1, 36, 12), False),
    }
    for name, ((x, y, z), expect) in checks.items():
        found = solid_at(body, x, y, z)
        if found != expect:
            raise SystemExit(f"Case check failed: {name} at {(x, y, z)}")
    for name, part in parts.items():
        if part.volume() < 1000:
            raise SystemExit(f"{name} is empty")
        write_stl(HERE / name, part)


if __name__ == "__main__":
    main()
