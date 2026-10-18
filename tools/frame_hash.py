#!/usr/bin/env python3
"""Print frame index and a hash of the newest frame in one eye's frame file.

  frame_hash.py /dev/shm/vrdual-SESSION-EYE

Reads the header of common/vrdual_shm.h directly (no pairing with the other
eye), e.g. to see that the left eye still changes after the mirror stopped.
"""
import hashlib
import struct
import sys

SLOTS = 4
with open(sys.argv[1], "rb") as f:
    hdr = f.read(4096)
    magic, version, w, h, stride = struct.unpack_from("<5I", hdr, 0)
    slot_bytes, last = struct.unpack_from("<QQ", hdr, 32)
    if magic != 0x56524455 or not last:
        sys.exit("no frame")
    frame = last - 1
    seq, fr, _ = struct.unpack_from("<QQQ", hdr, 48 + 24 * (frame % SLOTS))
    f.seek(4096 + (frame % SLOTS) * slot_bytes)
    px = f.read(h * stride)
print(frame, hashlib.sha1(px).hexdigest()[:12])
