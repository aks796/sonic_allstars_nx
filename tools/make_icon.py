#!/usr/bin/env python3
"""make_icon.py -- the launcher's icon (launcher/icon.jpg) from a picture.

Scales the picture to a 256x256 JPEG, as the NACP wants, and writes it to
launcher/icon.jpg and icon.jpg (the README's). The picture should be square.

  python3 tools/make_icon.py picture.png
"""
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUTS = [os.path.join(HERE, "..", "launcher", "icon.jpg"), os.path.join(HERE, "..", "icon.jpg")]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1].strip())
    img = Image.open(sys.argv[1]).convert("RGB").resize((256, 256), Image.LANCZOS)
    for out in OUTS:
        img.save(out, "JPEG", quality=95)
        print("wrote", os.path.normpath(out))


if __name__ == "__main__":
    main()
