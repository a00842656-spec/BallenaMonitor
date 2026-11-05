// Envio de la sesion a Firestore (la base de datos que lee la app de FlutterFlow).
// Basado en el codigo CODIGOFIRESTORE.ino del equipo: mismo proyecto, misma
// coleccion "consultas" y mismos campos (pacienteID, fecha, onda).
//
// Como funciona (ENVIAR):
//   1. La pantalla se conecta al hotspot (WIFI_SSID / WIFI_PASS) SOLO durante el envio.
//   2. Pide la hora a internet (NTP) -> "fecha" = momento en que se presiono GRABAR.
//   3. POST a Firestore: https://firestore.googleapis.com/v1/projects/<proyecto>/databases/(default)/documents/consultas
//   4. Se desconecta y regresa al canal de ESP-NOW (tortuga y ESP32 EMG).
// Mientras envia (~5-10 s) la tortuga y la EMG no reciben: la tortuga se detiene sola.
// Todo corre en otra tarea: la pantalla no se congela.

#pragma once

#include <Arduino.h>

// ---- Configuracion (igual que CODIGOFIRESTORE.ino) ------------------------------
#define FIRESTORE_WIFI_SSID "IPhone"     // nombre EXACTO del hotspot (mayusculas/espacios se corrigen solos)
#define FIRESTORE_WIFI_PASS "A00842656"
#define FIRESTORE_PROJECT_ID "emg-display"
#define FIRESTORE_COLLECTION "consultas"

// true = al encender, conectarse un momento al hotspot para tomar la hora
// (asi GRABAR ya tiene fecha). Si no hay hotspot, sigue igual (la hora se toma al ENVIAR).
constexpr bool FIRESTORE_SYNC_CLOCK_AT_BOOT = true;

enum FirestoreState : uint8_t { FS_IDLE, FS_BUSY, FS_OK, FS_FAIL };

void firestoreBegin();   // en setup(), despues de esp_now_init()

// Toma los datos de la ultima sesion (sessionLogSend ya llamado) y los sube.
// grabarMillis = millis() cuando se presiono GRABAR. false si ya esta enviando o no hay datos.
bool firestoreStartUpload(const char *pacienteId, int sesionPaciente, uint32_t grabarMillis);

FirestoreState firestoreState();
bool firestoreBusy();
bool firestoreHasUnsent();          // el ultimo envio fallo (ENVIAR otra vez = reintentar)
bool firestoreRetry();              // reintenta el ultimo envio fallido
const char *firestoreMessage();     // texto del ultimo resultado (para la pantalla)
uint32_t firestoreResultId();       // aumenta cada vez que termina un envio (OK o error)
