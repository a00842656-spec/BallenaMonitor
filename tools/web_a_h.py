"""Convierte tools/espejo.html en espejo_web.h (la pagina que sirve la pantalla por Wi-Fi).

Uso, desde la carpeta BallenaMonitor (despues de cambiar tools/espejo.html):
    python tools/web_a_h.py
"""
import os

aqui = os.path.dirname(os.path.abspath(__file__))
html = open(os.path.join(aqui, "espejo.html"), encoding="utf-8").read()
assert ")ESPEJO\"" not in html
salida = os.path.join(aqui, "..", "espejo_web.h")
with open(salida, "w", encoding="utf-8", newline="\n") as f:
    f.write("// Generado por tools/web_a_h.py a partir de tools/espejo.html: no editar a mano.\n")
    f.write("#pragma once\n#include <pgmspace.h>\n\n")
    f.write('static const char ESPEJO_HTML[] PROGMEM = R"ESPEJO(')
    f.write(html)
    f.write(')ESPEJO";\n')
print("listo:", os.path.normpath(salida), len(html.encode("utf-8")), "bytes")
