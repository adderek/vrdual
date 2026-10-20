#!/usr/bin/env python3
"""Print frame index and a hash of the newest frame in one eye's frame file.

  frame_hash.py /dev/shm/vrdual-SESSION-EYE            frame index, pixel hash
  frame_hash.py --cursor /dev/shm/vrdual-SESSION-EYE   cursor: x y w h serial

Reads the header of common/vrdual_shm.h directly (no pairing with the other
eye), e.g. to see that the left eye still changes after the mirror stopped.
"""
import hashlib
import struct
import sys

SLOTS = 4
HDR_BYTES = 4096 + 64 * 64 * 4  # VRDUAL_HDR_BYTES (v4)
CURSOR = 304                    # offsetof(vrdual_shm_header, cursor_seq), static_assert in the layer
cursor = sys.argv[1] == "--cursor"
with open(sys.argv[-1], "rb") as f:
    hdr = f.read(4096)
    magic, version, w, h, stride = struct.unpack_from("<5I", hdr, 0)
    slot_bytes, last = struct.unpack_from("<QQ", hdr, 32)
    if magic != 0x56524455 or not last:
        sys.exit("no frame")
    if version != 4:
        sys.exit("frame file version %d, expected 4" % version)
    if cursor:
        seq, serial, cx, cy, cw, ch = struct.unpack_from("<IIiiII", hdr, CURSOR)
        print(cx, cy, cw, ch, serial)
        sys.exit(0)
    frame = last - 1
    seq, fr, _ = struct.unpack_from("<QQQ", hdr, 48 + 24 * (frame % SLOTS))
    f.seek(HDR_BYTES + (frame % SLOTS) * slot_bytes)
    px = f.read(h * stride)
print(frame, hashlib.sha1(px).hexdigest()[:12])
