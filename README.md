# Ballena / Tortuga - pantalla WT32-SC01 Plus

UI de SquareLine Studio (LVGL 8.3.11) que controla la tortuga por ESP-NOW.
El receptor (XIAO ESP32-C6 + motor) está en el repo `tortuga-motor-test`.

## Compilar y subir

Requiere, en la carpeta de librerías de Arduino (`Documents/Arduino/libraries`):

- `lvgl` **8.3.11** y `LovyanGFX`
- `lv_conf.h` → copiar `extras/lv_conf.h` al lado de la carpeta `lvgl`
  (color 16 bits sin swap, Montserrat 16/20/26, `LV_TICK_CUSTOM 1` con `millis()`).

```sh
arduino-cli compile -b esp32:esp32:wt32-sc01-plus -u -p COM11 .
```

## ESP-NOW

- `motor_protocol.h` debe ser idéntico al del repo `tortuga-motor-test`.
- `turtleAddress` = MAC del XIAO (la imprime su Serial al arrancar).
- Mientras una flecha está presionada se reenvía el comando cada 100 ms; al soltarla se manda STOP.

## Nube de colores (MODO JUEGO)

La nube está dibujada dentro del fondo `ui_img_4_png.c`. Su relleno es una máscara aparte
(`ui_img_nube.c`, formato ALPHA_8BIT) que se pinta con `img_recolor` en `generateRandomColor()`.
Si cambias el fondo en SquareLine, regenera la máscara:

```sh
python tools/make_nube_mask.py
```
