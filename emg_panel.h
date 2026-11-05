// Panel de EMG en pantalla (creado por codigo, encima de las pantallas de SquareLine).
// Se muestra solo en MODO JUEGO y MODO REHABILITACION.
//
//   [ - ]  [==== barra RMS ====|umbral   ]  [ + ]  [CAL]
//            EMG 85 mV  |  umbral 60 mV
//
//   - Barra: envolvente RMS. Se pone verde cuando hay activacion.
//     La linea blanca es el umbral (siempre al 40 % de la barra).
//   - [-] / [+]: bajar / subir el umbral 10 %.
//   - [CAL]: calibracion (3 s relajado + 3 s apretando).
//   - Tocar el panel: muestra / oculta la grafica de los ultimos 5 s.
//   - Arrastrar el panel: moverlo a otro lugar (se guarda la posicion).

#pragma once

#include <lvgl.h>

void emgPanelCreate();
// Llamar en loop(). visible = la pantalla actual es MODO JUEGO o REHABILITACION
void emgPanelUpdate(bool visible);
