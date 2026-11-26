// EMG_Emisor: ESP32 que lee el circuito EMG (pin D34) y manda la senal a la
// pantalla (WT32-SC01 Plus) por ESP-NOW. No necesita cables hacia la pantalla.
//
// Es la MISMA cadena del codigo "EMG facial v2" del equipo:
//   ADC 1 kHz (x4 promediado, mV calibrados) -> notch 60 Hz -> pasa-altas 20 Hz
//   -> pasa-bajas 300 Hz -> rechazo de artefactos (|y| > 1 V = 0)
//   -> RMS 150 ms -> suavizado (alpha 0.02)
// Cada 20 ms manda {RMS, pico, nivel DC, saturado} a la pantalla.
// La pantalla decide si hay contraccion (umbral, CAL, barra) y mueve la tortuga.
//
// Conexiones (iguales que antes): circuito -> 3V3, GND, D35. Alimentacion por USB
// (o 5 V del regulador en VIN y GND).
//
// Serial Monitor / Plotter (115200): 'p' = plot on/off (mismo formato del equipo), '?' = estado.
// Placa en Arduino: "ESP32 Dev Module" (o la que usaban). Archivo motor_protocol.h
// junto a este .ino (identico al de la pantalla).

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <math.h>

#include "motor_protocol.h"

// ---- Configuracion ------------------------------------------------------------
constexpr int EMG_PIN = 35;   // ADC1: funciona con Wi-Fi / ESP-NOW encendido

// A quien mandar. Broadcast funciona sin saber la MAC de la pantalla.
// (Opcional) poner la MAC que imprime la pantalla al arrancar: "[PANTALLA] MAC: ..."
uint8_t screenAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

constexpr uint32_t FS_HZ = 1000;
constexpr int OVERSAMPLE = 4;
constexpr uint32_t RMS_WINDOW_MS = 150;
constexpr float ALPHA_RMS = 0.02f;
constexpr float ARTIFACT_MV = 1000.0f;
constexpr uint32_t WARMUP_MS = 1000;
constexpr float DC_TAU_S = 0.5f;
constexpr float SAT_LOW_MV = 20.0f;
constexpr float SAT_HIGH_MV = 3050.0f;
constexpr float PLOT_THRESHOLD_MV = 30.0f;   // solo para la linea del plotter

// ---- Bateria de la ballena (opcional) ---------------------------------------------
// BAT_PIN: pin ADC1 con un divisor de voltaje desde el + de la bateria.
//   -1 = no conectado (la pantalla muestra "--").
//   Pines ADC1 libres del ESP32: 32, 33, 35, 36 (VP), 39 (VN). NO usar ADC2 (Wi-Fi).
// Divisor sugerido para una bateria de 9 V: + bateria -> 100k -> PIN -> 33k -> GND
//   (9 V -> 2.2 V en el pin; el ADC lee hasta ~3.1 V)
// BAT_DIVIDER = (R_arriba + R_abajo) / R_abajo = (100k + 33k) / 33k
// BAT_V_FULL / BAT_V_EMPTY: voltaje de la bateria llena / descargada (cambiar segun la bateria)
constexpr int BAT_PIN = -1;
constexpr float BAT_DIVIDER = (100.0f + 33.0f) / 33.0f;
constexpr float BAT_V_FULL = 9.0f;
constexpr float BAT_V_EMPTY = 7.0f;

constexpr uint32_t RMS_N = FS_HZ * RMS_WINDOW_MS / 1000;
constexpr uint32_t WARMUP_SAMPLES = FS_HZ * WARMUP_MS / 1000;
constexpr uint32_t SAT_HOLD_SAMPLES = FS_HZ / 2;

// ---- Biquads (fs = 1000 Hz), iguales al codigo del equipo -------------------------
struct Biquad {
  float b0, b1, b2, a1, a2;
  float x1, x2, y1, y2;
  float process(float x) {
    float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x; y2 = y1; y1 = y;
    return y;
  }
};
static Biquad notch60 = {0.97618f, -1.81526f, 0.97618f, -1.80379f, 0.94090f, 0, 0, 0, 0};
static Biquad hp20    = {0.91497f, -1.82994f, 0.91497f, -1.82272f, 0.83718f, 0, 0, 0, 0};
static Biquad lp300   = {0.39134f,  0.78268f, 0.39134f,  0.36953f, 0.19582f, 0, 0, 0, 0};

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static float sq[RMS_N];
static double sqSum = 0;
static uint32_t sqIdx = 0;
static float rmsSuave = 0;
static float dcMean = -1;
static uint32_t satHold = 0;
static uint32_t nSamples = 0;
static float peakSend = 0;   // pico |y| desde el ultimo mensaje
static float peakPlot = 0;   // pico |y| desde la ultima linea del plotter
static volatile uint32_t batAdcMv = 0;   // lectura del pin de bateria (mV en el pin)

static void sampleCb(void *) {
  uint32_t acc = 0;
  for (int i = 0; i < OVERSAMPLE; i++) acc += analogReadMilliVolts(EMG_PIN);
  float v = (float)acc / OVERSAMPLE;

  if (dcMean < 0) dcMean = v;
  dcMean += (v - dcMean) * (1.0f / (FS_HZ * DC_TAU_S));

  float y = lp300.process(hp20.process(notch60.process(v)));
  float ym = (fabsf(y) > ARTIFACT_MV) ? 0 : y;

  float s = ym * ym;
  sqSum += s - sq[sqIdx];
  sq[sqIdx] = s;
  if (++sqIdx >= RMS_N) {
    sqIdx = 0;
    double t = 0;
    for (uint32_t i = 0; i < RMS_N; i++) t += sq[i];
    sqSum = t;
  }
  float rms = sqrtf((float)(sqSum > 0 ? sqSum / RMS_N : 0));
  rmsSuave += ALPHA_RMS * (rms - rmsSuave);

  if (v <= SAT_LOW_MV || v >= SAT_HIGH_MV) satHold = SAT_HOLD_SAMPLES;
  else if (satHold > 0) satHold--;

  // bateria: una lectura por segundo, en la misma tarea que la EMG (el ADC no se usa en paralelo)
  if (BAT_PIN >= 0 && nSamples % FS_HZ == 0) {
    uint32_t b = 0;
    for (int i = 0; i < 4; i++) b += analogReadMilliVolts(BAT_PIN);
    batAdcMv = b / 4;
  }

  portENTER_CRITICAL(&mux);
  nSamples++;
  if (fabsf(ym) > peakSend) peakSend = fabsf(ym);
  if (fabsf(ym) > peakPlot) peakPlot = fabsf(ym);
  portEXIT_CRITICAL(&mux);
}

static esp_timer_handle_t timer = nullptr;
static bool espNowOk = false;
static uint8_t seq = 0;
static uint32_t sentOk = 0, sentFail = 0;
static bool plotOn = false;

static uint16_t clampU16(float x) {
  if (x < 0) return 0;
  if (x > 65535) return 65535;
  return (uint16_t)lroundf(x);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  analogReadResolution(12);
  analogSetPinAttenuation(EMG_PIN, ADC_11db);
  analogReadMilliVolts(EMG_PIN);
  if (BAT_PIN >= 0) analogSetPinAttenuation(BAT_PIN, ADC_11db);

  // ESP-NOW en el mismo canal que la pantalla y la tortuga
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() == ESP_OK) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, screenAddress, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;
    espNowOk = esp_now_add_peer(&peer) == ESP_OK;
  }
  Serial.printf("[EMG_Emisor] MAC %s, canal %u, ESP-NOW %s. Pin D%d. 'p' = plot, '?' = estado\n",
                WiFi.macAddress().c_str(), ESPNOW_CHANNEL, espNowOk ? "OK" : "ERROR", EMG_PIN);

  esp_timer_create_args_t args = {};
  args.callback = sampleCb;
  args.name = "emg";
  esp_timer_create(&args, &timer);
  esp_timer_start_periodic(timer, 1000000ULL / FS_HZ);
}

void loop() {
  static uint32_t lastSend = 0;
  uint32_t now = millis();

  if (now - lastSend >= EMG_MSG_PERIOD_MS) {
    lastSend = now;
    EmgMsg m;
    portENTER_CRITICAL(&mux);
    float pk = peakSend;
    peakSend = 0;
    uint32_t n = nSamples;
    portEXIT_CRITICAL(&mux);
    m.magic = EMG_MSG_MAGIC;
    m.seq = seq++;
    m.rms_x10 = clampU16(rmsSuave * 10.0f);
    m.peak_x10 = clampU16(pk * 10.0f);
    m.input_mv = clampU16(dcMean);
    m.flags = (satHold > 0 ? 1 : 0) | (n < WARMUP_SAMPLES ? 2 : 0);
    if (espNowOk) {
      if (esp_now_send(screenAddress, (uint8_t *)&m, sizeof(m)) == ESP_OK) sentOk++;
      else sentFail++;
    }
  }

  // bateria de la ballena -> pantalla
  static uint32_t lastBat = 0;
  if (BAT_PIN >= 0 && espNowOk && batAdcMv > 0 && now - lastBat >= BALLENA_BAT_PERIOD_MS) {
    lastBat = now;
    float v = batAdcMv / 1000.0f * BAT_DIVIDER;
    float pct = (v - BAT_V_EMPTY) / (BAT_V_FULL - BAT_V_EMPTY) * 100.0f;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    BallenaBatMsg b;
    b.magic = BALLENA_BAT_MAGIC;
    b.battery_mv = clampU16(v * 1000.0f);
    b.battery_pct = (uint8_t)lroundf(pct);
    esp_now_send(screenAddress, (uint8_t *)&b, sizeof(b));
  }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'p' || c == 'P') {
      plotOn = !plotOn;
      Serial.printf("[EMG] plot %s\n", plotOn ? "ON" : "OFF");
    } else if (c == '?') {
      Serial.printf("[EMG] entrada=%.0f mV rms=%.1f mV saturado=%d enviados=%lu fallos=%lu\n", (double)dcMean,
                    (double)rmsSuave, satHold > 0, (unsigned long)sentOk, (unsigned long)sentFail);
      if (BAT_PIN >= 0) {
        Serial.printf("[BATERIA] pin %lu mV -> bateria %.2f V\n", (unsigned long)batAdcMv,
                      (double)(batAdcMv / 1000.0f * BAT_DIVIDER));
      }
    }
  }

  static uint32_t lastPlot = 0;
  if (plotOn && now - lastPlot >= 20) {
    lastPlot = now;
    portENTER_CRITICAL(&mux);
    float pk = peakPlot;
    peakPlot = 0;
    portEXIT_CRITICAL(&mux);
    Serial.printf("RMS_mV:%.1f,Pico_mV:%.1f,Estado:%d,Para activar:%.0f,Max:350,Min:20\n", (double)rmsSuave,
                  (double)pk, rmsSuave > PLOT_THRESHOLD_MV ? 200 : 0, (double)PLOT_THRESHOLD_MV);
  }
}
