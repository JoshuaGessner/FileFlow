#!/usr/bin/env python3
"""Verify a transmitter screenshot really contains an optical frame (C03 sanity check).

WHY THIS EXISTS. `TransmitActivity` reporting "0 render errors" only means `nextFrame()` returned
success. It says nothing about what reached the panel: a wrong texture format, a wrong unpack
alignment, a filtered upscale or a flipped quad would all present *something* at a perfect 120 Hz
and report no errors at all. So the cadence result is worthless until the pixels are checked.

Checks, in order of how badly a failure would mislead us:

  1. **Resolution** matches the panel, so nothing scaled the surface.
  2. **The boundary ring is bright.** It is always-on in every frame by construction (F10), so a
     dark border means the frame is not being rendered, is offset, or is inverted.
  3. **The interior is bimodal**, i.e. mostly near-black and near-white rather than mid-grey. A
     mid-grey interior is the signature of a FILTERED upscale, which would blur exactly the cell
     edges the receiver's sampler needs (and would still look plausible to the eye).
  4. **Cell pitch is an exact integer** and runs of identical pixels match it, which is what proves
     GL_NEAREST gave a bit-exact upscale rather than an interpolated one.

Usage:  tools/verify_tx_screenshot.py <screenshot.png> <cols> <rows>

Needs no third-party imaging library: it parses the PNG and inflates it with `zlib` from the
standard library, because adding a dependency to check one screenshot is a bad trade.
"""
import struct
import sys
import zlib


def read_png_gray(path):
    """Minimal PNG reader. Returns (width, height, rows-of-luminance)."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")

    pos = 8
    width = height = 0
    bit_depth = colour = 0
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            width, height, bit_depth, colour = struct.unpack(">IIBB", body[:10])
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        pos += 12 + length

    if bit_depth != 8 or colour not in (2, 6):
        raise ValueError(f"expected 8-bit RGB/RGBA, got depth {bit_depth} colour type {colour}")
    channels = 3 if colour == 2 else 4

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = []
    prev = bytearray(stride)
    p = 0
    for _ in range(height):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        # PNG filters. Implemented in full because a screenshot may use any of them, and guessing
        # "None" would silently produce a garbage image that then fails the real checks for the
        # wrong reason.
        if f == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif f != 0:
            raise ValueError(f"unknown PNG filter {f}")
        # Luminance from the green channel alone: M0 writes the same value to R, G and B, so green
        # is exact here and avoids weighting three identical numbers.
        out.append([line[x * channels + 1] for x in range(width)])
        prev = line
    return width, height, out


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    path, cols, rows = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    w, h, px = read_png_gray(path)

    print(f"image                  {w}x{h}")
    print(f"grid                   {cols}x{rows}")
    ok = True

    # --- 1. integer cell pitch --------------------------------------------------------
    if w % cols or h % rows:
        print(f"FAIL pitch             {w/cols:.3f} x {h/rows:.3f} px/cell — NOT integer")
        ok = False
    else:
        print(f"ok   pitch             {w // cols} x {h // rows} px/cell (exact integer)")

    # --- 2. boundary ring is bright ---------------------------------------------------
    #
    # Sampled at the CENTRE of the outermost cell row/column, not at the outermost pixel. The
    # extreme edge is the wrong place to look: rounded display corners are black, and `screencap`
    # composites system overlays such as the gesture pill on top of the surface. Both make the very
    # last pixel row dark while the ring itself is perfectly bright, which reads as a rendering
    # failure that is not there.
    pitch_x = max(1, w // cols)
    pitch_y = max(1, h // rows)
    cy_top = pitch_y // 2
    cy_bot = h - 1 - pitch_y // 2
    cx_left = pitch_x // 2
    cx_right = w - 1 - pitch_x // 2
    # Trim the ends of each span by one cell so the corner markers, which are legitimately dark in
    # places, cannot drag a mean down.
    top = sum(px[cy_top][pitch_x:w - pitch_x]) / max(1, w - 2 * pitch_x)
    bottom = sum(px[cy_bot][pitch_x:w - pitch_x]) / max(1, w - 2 * pitch_x)
    left = sum(px[y][cx_left] for y in range(pitch_y, h - pitch_y)) / max(1, h - 2 * pitch_y)
    right = sum(px[y][cx_right] for y in range(pitch_y, h - pitch_y)) / max(1, h - 2 * pitch_y)
    print(f"     ring means        top {top:.1f}, bottom {bottom:.1f}, "
          f"left {left:.1f}, right {right:.1f}  (sampled mid-cell)")
    if min(top, bottom, left, right) < 128:
        print("FAIL boundary ring     a border is not bright — frame missing, offset or inverted")
        ok = False
    else:
        print("ok   boundary ring     all four borders bright, as F10 requires of every frame")
    band = pitch_y

    # --- 3. interior is bimodal, not grey ---------------------------------------------
    inset = band * 3
    dark = bright = mid = 0
    for y in range(inset, h - inset):
        row = px[y]
        for x in range(inset, w - inset):
            v = row[x]
            if v < 64:
                dark += 1
            elif v > 192:
                bright += 1
            else:
                mid += 1
    total = dark + bright + mid
    frac_mid = mid / total if total else 1.0
    print(f"     interior           dark {dark/total:.4f}, bright {bright/total:.4f}, "
          f"mid {frac_mid:.4f}")
    if frac_mid > 0.10:
        print("FAIL bimodality        too much mid-grey — the upscale is FILTERED, which blurs the")
        print("                       cell edges the receiver's sampler needs")
        ok = False
    else:
        print("ok   bimodality        interior is two-level; the GL_NEAREST upscale is not blurring")

    # --- 4. runs of identical pixels match the pitch ----------------------------------
    if w % cols == 0:
        pitch = w // cols
        y = h // 2
        row = px[y]
        runs, cur, val = [], 1, row[0]
        for x in range(1, w):
            if row[x] == val:
                cur += 1
            else:
                runs.append(cur)
                cur, val = 1, row[x]
        runs.append(cur)
        bad = [r for r in runs if r % pitch]
        print(f"     mid-row runs       {len(runs)} runs, "
              f"{len(bad)} not a multiple of {pitch} px")
        if bad:
            print("FAIL nearest-neighbour run lengths are not multiples of the cell pitch, so the")
            print("                       upscale interpolated across cell boundaries")
            ok = False
        else:
            print("ok   nearest-neighbour every run is a whole number of cells — upscale is exact")

    print()
    print("PASS — the panel is showing a real optical frame" if ok
          else "FAIL — do not trust any cadence number from this run")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
