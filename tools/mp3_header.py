"""Convierte audio a un .h con un MP3 en PROGMEM, como los demas sonidos del sketch.

Todos los sonidos deben ser MP3 a 44100 Hz: el mezclador (musica + efectos) no
convierte frecuencias, y un sonido a otra frecuencia se oiria mas grave o agudo.

Uso (desde la carpeta del sketch):
    python tools/mp3_header.py correct.h correct          # re-codifica un .h existente
    python tools/mp3_header.py mi_sonido.wav nuevo        # crea nuevo.h desde cualquier audio
    python tools/mp3_header.py rojo.h rojo --sfx          # efecto: ver abajo

--sfx (para efectos y voces, no para la musica):
    - quita el silencio del inicio (si no, el sonido se oye con retraso)
    - sube el volumen hasta PEAK_DB sin saturar
    - mono (hay una sola bocina y pesa la mitad)
Requiere: pip install imageio-ffmpeg
"""
import os
import re
import subprocess
import sys
import tempfile

import imageio_ffmpeg

RATE = 44100
BITRATE = "128k"
SFX_BITRATE = "96k"
PEAK_DB = -2.0           # pico final de los efectos (dBFS)
SILENCE_DB = -40         # lo que esta por debajo al inicio se considera silencio
FFMPEG = imageio_ffmpeg.get_ffmpeg_exe()


def peak_db(path, filters):
    r = subprocess.run([FFMPEG, "-hide_banner", "-i", path, "-af", filters + ",volumedetect",
                        "-f", "null", "-"], capture_output=True, text=True)
    return float(re.search(r"max_volume: (-?[\d.]+) dB", r.stderr).group(1))


def read_header(path):
    s = open(path).read()
    body = s[s.index("{") + 1:s.rindex("}")]
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))


def write_header(path, name, data):
    with open(path, "w", newline="\n") as f:
        f.write(f"// array size is {len(data)}\n")
        f.write(f"static const byte {name}[] PROGMEM  = {{\n")
        for i in range(0, len(data), 16):
            f.write("  " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ", \n")
        f.write("};\n")


def main():
    src, name = sys.argv[1], sys.argv[2]
    sfx = "--sfx" in sys.argv[3:]
    with tempfile.TemporaryDirectory() as tmp:
        if src.endswith(".h"):
            inp = os.path.join(tmp, "in.mp3")
            open(inp, "wb").write(read_header(src))
        else:
            inp = src
        out = os.path.join(tmp, "out.mp3")
        args = ["-ar", str(RATE), "-b:a", BITRATE]
        if sfx:
            trim = f"silenceremove=start_periods=1:start_threshold={SILENCE_DB}dB"
            gain = PEAK_DB - peak_db(inp, trim)
            args = ["-af", f"{trim},volume={gain:.2f}dB", "-ac", "1", "-ar", str(RATE), "-b:a", SFX_BITRATE]
        subprocess.run([FFMPEG, "-y", "-loglevel", "error", "-i", inp, "-map_metadata", "-1", *args, out],
                       check=True)
        data = open(out, "rb").read()
    write_header(f"{name}.h", name, data)
    print(f"{name}.h: {len(data)} bytes, {RATE} Hz")


if __name__ == "__main__":
    main()
