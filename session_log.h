// Registro de la sesion de rehabilitacion (GRABAR) y envio por Wi-Fi (ENVIAR).
//
// GRABAR: borra lo anterior y empieza a guardar desde ese momento:
//   - envolvente RMS de la EMG cada 50 ms (20 Hz)
//   - cada repeticion (tiempo, duracion, pico RMS, latencia)
//   - pausas / continuar
// ENVIAR: deja de grabar y publica los datos para la aplicacion.
//
// Como recibe la aplicacion los datos (Wi-Fi):
//   La pantalla crea su propia red Wi-Fi (no necesita router ni internet):
//       red:        WIFI_AP_SSID      (ver session_log.cpp)
//       contrasena: WIFI_AP_PASS
//       direccion:  http://192.168.4.1
//   La aplicacion se conecta a esa red y pide:
//       GET /sync?epoch=1762300000&tz_min=-360
//                         -> la app le da la fecha/hora actual (la pantalla no tiene reloj).
//                            Llamarlo al conectarse, ANTES de que el fisioterapeuta presione GRABAR.
//       GET /estado       -> {"grabando":..,"listo":..,"id":..}  (consultar cada 1-2 s)
//       GET /sesion.json  -> todos los datos de la ultima sesion enviada
//       GET /sesion.csv   -> la envolvente en CSV (para Excel)
//       GET /             -> pagina simple para verlo en un navegador
//   Cada vez que se presiona ENVIAR, "id" aumenta: asi la app sabe que hay datos nuevos.
//
// La red usa el mismo canal que ESP-NOW, asi la tortuga sigue funcionando.

#pragma once

#include <Arduino.h>

void sessionLogBegin();   // en setup(), despues de esp_now_init()
void sessionLogLoop();    // en loop()

// pacienteId: "PAC-101" etc. Se guarda junto con la fecha y hora de inicio.
void sessionLogStart(int objetivo, const char *pacienteId);
// manual = true: repeticion sumada con el boton CONTINUAR (no por la EMG)
void sessionLogRep(uint32_t onsetMs, uint32_t durMs, float peakMv, int32_t latencyMs, bool manual);
void sessionLogPause(bool paused);
// Termina la grabacion y publica los datos. Devuelve false si no habia nada grabado.
// sesionPaciente: numero de sesion de ESE paciente (PAC-101 #1, #2, ...)
bool sessionLogSend(int objetivo, int repeticiones, bool completada, int sesionPaciente);

bool sessionLogRecording();
bool sessionLogClockValid();   // true si la app ya mando la hora (/sync)
uint32_t sessionLogElapsedMs();
uint32_t sessionLogId();
const char *sessionLogApSsid();
