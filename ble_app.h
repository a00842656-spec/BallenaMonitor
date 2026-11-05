// Bluetooth (BLE) entre la pantalla y la aplicacion (FlutterFlow).
//
// La pantalla es un periferico BLE llamado "Ballena-EMG" con el servicio
// "Nordic UART" (NUS), el mas usado para mandar texto por BLE:
//
//   Servicio  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   RX        6E400002-B5A3-F393-E0A9-E50E24DCCA9E   app -> pantalla (WRITE)
//   TX        6E400003-B5A3-F393-E0A9-E50E24DCCA9E   pantalla -> app (NOTIFY)
//
// Todo es texto. Cada mensaje es UNA linea de JSON que termina en '\n'.
// Un mensaje largo llega en varios pedazos (notificaciones): la app los junta
// hasta encontrar '\n' y entonces lo convierte con jsonDecode.
//
// La app escribe en RX (comandos de texto):
//   SYNC <epoch> <tz_min>   pone la fecha/hora, p. ej. "SYNC 1762360000 -360"
//   ESTADO                  responde {"tipo":"estado",...}
//   GET                     vuelve a mandar la ultima sesion
//
// La pantalla manda por TX:
//   {"tipo":"hola",...}      al conectarse
//   {"tipo":"reloj",...}     respuesta a SYNC
//   {"tipo":"estado",...}    respuesta a ESTADO, y al presionar GRABAR
//   {"tipo":"sesion",...}    al presionar ENVIAR (mismos campos que /sesion.json,
//                            sin "envolvente_mV")
//
// Si la app no esta conectada al presionar ENVIAR, la sesion se guarda y se
// manda sola cuando la app se conecte.

#pragma once

#include <Arduino.h>

constexpr const char *BLE_DEVICE_NAME = "Ballena-EMG";

void bleAppBegin();
void bleAppLoop();           // en loop()
bool bleAppConnected();
bool bleAppSending();        // todavia mandando pedazos de un mensaje
void bleAppSendEstado();     // (GRABAR) avisa a la app
// (ENVIAR) manda la sesion. Devuelve true si la app esta conectada (si no, queda pendiente)
bool bleAppSendSession();
