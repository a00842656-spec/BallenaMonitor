// ============================================================================
//  Bateria de la tortuga -> pantalla (ESP-NOW)      [para el XIAO ESP32-C6]
// ============================================================================
// Como usarlo en el proyecto de la TORTUGA:
//   1. Copia este archivo y el motor_protocol.h NUEVO (el mismo de la pantalla)
//      a la carpeta del proyecto de la tortuga.
//   2. En el .ino de la tortuga:
//        #include "tortuga_bateria.h"
//        en setup(), DESPUES de esp_now_init():   bateriaBegin();
//        en loop():                               bateriaLoop();
//   3. Hardware: divisor de voltaje de la bateria al pin BATT_PIN
//
//        BAT+ ──[ R1 100k ]──┬── BATT_PIN (A0)
//                            │
//                         [ R2 100k ]
//                            │
//        BAT- / GND ─────────┴── GND
//
//      Con R1 = R2 el pin ve la mitad del voltaje (4.2 V -> 2.1 V, seguro para 3.3 V).
//      NUNCA conectes la bateria directo al pin.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include "motor_protocol.h"

// ---- Ajustar a tu hardware ----
constexpr int BATT_PIN = A0;               // pin ADC con el divisor (XIAO ESP32-C6: A0 = GPIO0)
constexpr float BATT_DIVIDER = 2.0f;       // (R1 + R2) / R2  -> 2.0 con dos resistencias iguales
constexpr float BATT_CAL = 1.00f;          // correccion fina: (voltaje medido con multimetro) / (voltaje que imprime)

// Tipo de bateria. LiPo 1S (3.7 V nominal) por defecto.
// Si usas pilas AA/AAA en serie, cambia BATT_IS_LIPO a false y ajusta:
//   2 pilas: FULL 3200, EMPTY 2000   |   3 pilas: FULL 4800, EMPTY 3000   |   4 pilas: FULL 6400, EMPTY 4000
// (con 3 o 4 pilas el divisor debe ser mayor: R1 = 220k, R2 = 100k -> BATT_DIVIDER 3.2)
constexpr bool BATT_IS_LIPO = true;
constexpr uint16_t BATT_FULL_MV = 4200;
constexpr uint16_t BATT_EMPTY_MV = 3300;

static uint8_t bateriaBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static float bateriaFiltMv = 0;

// Curva de descarga aproximada de una LiPo 1S (mV -> %)
static uint8_t lipoPercent(float mv) {
  static const uint16_t v[] = {3300, 3500, 3600, 3700, 3750, 3800, 3850, 3900, 4000, 4100, 4200};
  static const uint8_t p[] = {0, 5, 10, 20, 30, 40, 50, 60, 75, 90, 100};
  if (mv <= v[0]) return 0;
  for (int i = 1; i < 11; i++) {
    if (mv <= v[i]) return p[i - 1] + (uint8_t)((mv - v[i - 1]) * (p[i] - p[i - 1]) / (v[i] - v[i - 1]));
  }
  return 100;
}

static uint8_t batteryPercent(float mv) {
  if (BATT_IS_LIPO) return lipoPercent(mv);
  float pct = (mv - BATT_EMPTY_MV) * 100.0f / (BATT_FULL_MV - BATT_EMPTY_MV);
  return (uint8_t)constrain(pct, 0.0f, 100.0f);
}

static float readBatteryMv() {
  // promedio de 16 lecturas (el PWM del motor mete ruido)
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BATT_PIN);
  return (sum / 16.0f) * BATT_DIVIDER * BATT_CAL;
}

void bateriaBegin() {
  analogSetPinAttenuation(BATT_PIN, ADC_11db);
  // peer broadcast: la tortuga no necesita saber la MAC de la pantalla
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, bateriaBroadcast, 6);
  p.channel = ESPNOW_CHANNEL;
  p.encrypt = false;
  if (!esp_now_is_peer_exist(bateriaBroadcast)) esp_now_add_peer(&p);
  bateriaFiltMv = readBatteryMv();
}

void bateriaLoop() {
  static uint32_t last = 0;
  if (millis() - last < TELEMETRY_PERIOD_MS) return;
  last = millis();

  // filtro lento: el voltaje baja cuando el motor arranca, no queremos que el % salte
  bateriaFiltMv += (readBatteryMv() - bateriaFiltMv) * 0.2f;

  TelemetryMsg m;
  m.magic = TELEMETRY_MSG_MAGIC;
  m.battery_mv = (uint16_t)bateriaFiltMv;
  m.battery_pct = batteryPercent(bateriaFiltMv);
  esp_now_send(bateriaBroadcast, (uint8_t *)&m, sizeof(m));
  Serial.printf("Bateria: %u mV (%u%%)\n", m.battery_mv, m.battery_pct);
}
