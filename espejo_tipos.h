// Tipos del espejo de pantalla (ver "ESPEJO DE PANTALLA" en BallenaMonitor.ino).
// Van en un .h porque el Arduino IDE escribe los prototipos de las funciones del .ino
// arriba de todo, antes de cualquier struct declarado en el .ino.
#pragma once
#include <stdint.h>

// Zonas cambiadas que se guardan por separado (si hay mas, se juntan las mas cercanas).
// Asi un cambio arriba a la izquierda y otro abajo a la derecha no obligan a mandar toda la pantalla.
#define ESPEJO_MAX_ZONAS 8

struct EspejoZona {
    int16_t x1, y1, x2, y2;
};

// Una "salida" = un lugar al que se manda la imagen (USB o Wi-Fi). Cada una lleva sus propias zonas pendientes.
struct EspejoSalida {
    const char *nombre;
    volatile bool on;
    volatile uint32_t ultimoM;
    EspejoZona suc[ESPEJO_MAX_ZONAS];     // zonas cambiadas que faltan
    uint8_t nSuc;
    int16_t envX1, envX2, envY, envY2;    // zona que se esta mandando (envY > envY2 = nada)
    char remBuf[7];                       // toque "X" + 7 caracteres
    uint8_t remLen;
    bool remLeyendo;
    uint32_t remInicioMs;
};
