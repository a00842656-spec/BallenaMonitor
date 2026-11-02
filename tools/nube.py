import re
from collections import deque

import numpy as np
from PIL import Image, ImageFilter

SRC = "ui_img_4_png.c"
OUT = "ui_img_nube.c"
W, H = 480, 320
SEED = (160, 240)   # (y, x) punto dentro de la nube
WALL_LUM = 160      # brillo a partir del cual un pixel es el contorno neon
BLUR = 1.2          # borde suave para que se funda con el brillo del contorno
PAD = 3


def load_rgb565(path):
    s = open(path).read()
    body = s[s.index("_map[] = {") + 10:s.index("};")]
    b = np.array([int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body)], dtype=np.uint16)
    px = ((b[1::2] << 8) | b[0::2])[:W * H].reshape(H, W)  # RGB565 little-endian (LV_COLOR_16_SWAP 0)
    r = ((px >> 11) & 31) * 255 // 31
    g = ((px >> 5) & 63) * 255 // 63
    bl = (px & 31) * 255 // 31
    return np.dstack([r, g, bl]).astype(float)


def flood(wall, seed):
    m = np.zeros_like(wall)
    q = deque([seed])
    m[seed] = True
    while q:
        y, x = q.popleft()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ny, nx = y + dy, x + dx
            if 0 <= ny < H and 0 <= nx < W and not m[ny, nx] and not wall[ny, nx]:
                m[ny, nx] = True
                q.append((ny, nx))
    return m


def main():
    lum = load_rgb565(SRC).mean(axis=2)
    inside = flood(lum > WALL_LUM, SEED)
    if inside.sum() > W * H // 3:
        raise SystemExit("El relleno se salio de la nube: revisa SEED / WALL_LUM")

    alpha = np.array(Image.fromarray((inside * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(BLUR)))
    ys, xs = np.nonzero(inside)
    x0, y0 = max(xs.min() - PAD, 0), max(ys.min() - PAD, 0)
    x1, y1 = min(xs.max() + PAD, W - 1), min(ys.max() + PAD, H - 1)
    crop = alpha[y0:y1 + 1, x0:x1 + 1]
    h, w = crop.shape

    rows = []
    for row in crop:
        rows.append("  " + ", ".join(f"0x{v:02x}" for v in row) + ",")

    with open(OUT, "w", newline="\n") as f:
        f.write(f"""// Generado por tools/make_nube_mask.py - no editar a mano
// Mascara del interior de la nube de MODO JUEGO, posicion en pantalla: x={x0} y={y0}

#include "ui.h"

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t ui_img_nube_map[] = {{
{chr(10).join(rows)}
}};

const lv_img_dsc_t ui_img_nube = {{
  .header.always_zero = 0,
  .header.w = {w},
  .header.h = {h},
  .data_size = {w * h},
  .header.cf = LV_IMG_CF_ALPHA_8BIT,
  .data = ui_img_nube_map,
}};

// Esquina superior izquierda de la mascara sobre el fondo de 480x320
const lv_coord_t ui_img_nube_x = {x0};
const lv_coord_t ui_img_nube_y = {y0};
""")
    print(f"{OUT}: {w}x{h} en x={x0} y={y0}")


if __name__ == "__main__":
    main()
