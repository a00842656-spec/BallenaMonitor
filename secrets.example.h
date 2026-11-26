// Plantilla de secrets.h (datos privados que NO se suben a GitHub).
//
// 1. Copiar este archivo como secrets.h (en la misma carpeta que BallenaMonitor.ino).
// 2. Poner el nombre y la contrasena del hotspot que usa ENVIAR (2.4 GHz).
// secrets.h esta en .gitignore: cada quien tiene el suyo en su computadora.
#pragma once

#define FIRESTORE_WIFI_SSID "nombre-del-hotspot"   // nombre EXACTO (mayusculas/espacios se corrigen solos)
#define FIRESTORE_WIFI_PASS "contrasena-del-hotspot"