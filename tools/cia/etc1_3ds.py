"""3DS ETC1 textures: decode to / encode from Pillow images (8x8 tiles of 4x4 blocks, blocks
stored as little-endian 64-bit words, rows bottom-up as the 3DS GPU expects)."""
import struct
from PIL import Image
import etcpak, texture2ddecoder

def _tile_order(w, h):
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for by, bx in ((0, 0), (0, 4), (4, 0), (4, 4)):
                yield tx + bx, ty + by

def decode(data, w, h, flip=False):
    # reorder 3DS blocks into a standard row-major ETC1 stream (big-endian blocks)
    blocks = {}
    for i, (x, y) in enumerate(_tile_order(w, h)):
        blk = data[i * 8:i * 8 + 8]
        blocks[(x, y)] = blk[::-1]
    std = b''.join(blocks[(x, y)] for y in range(0, h, 4) for x in range(0, w, 4))
    rgba = texture2ddecoder.decode_etc1(std, w, h)  # BGRA
    im = Image.frombytes('RGBA', (w, h), rgba, 'raw', 'BGRA')
    return im.transpose(Image.FLIP_TOP_BOTTOM) if flip else im

def encode(im, flip=False):
    im = im.convert('RGBA')
    if flip:
        im = im.transpose(Image.FLIP_TOP_BOTTOM)
    w, h = im.size
    rgba = im.tobytes('raw', 'RGBA')  # etcpak takes RGBA; texture2ddecoder returns BGRA
    std = etcpak.compress_etc1_rgb(rgba, w, h)  # row-major 4x4 blocks
    bw = w // 4
    out = bytearray()
    for x, y in _tile_order(w, h):
        i = (y // 4) * bw + (x // 4)
        out += std[i * 8:i * 8 + 8][::-1]
    return bytes(out)
