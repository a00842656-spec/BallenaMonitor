// Tipos del espejo de pantalla (ver "ESPEJO DE PANTALLA" en BallenaMonitor.ino).
// Van en un .h porque el Arduino IDE escribe los prototipos de las funciones del .ino
// arriba de todo, antes de cualquier struct declarado en el .ino.
#pragma once
#include <stdint.h>

// Una "salida" = un lugar al que se manda la imagen (USB o Wi-Fi). Cada una lleva su propia zona pendiente.
struct EspejoSalida {
    const char *nombre;
    bool on;
    uint32_t ultimoM;
    int16_t sucX1, sucY1, sucX2, sucY2;   // zona cambiada, falta mandar (sucX2 < sucX1 = nada)
    int16_t envX1, envX2, envY, envY2;    // zona que se esta mandando (envY > envY2 = nada)
    char remBuf[7];                       // toque "X" + 7 caracteres
    uint8_t remLen;
    bool remLeyendo;
    uint32_t remInicioMs;
};
