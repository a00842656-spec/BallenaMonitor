// Ver emg.h
#include "emg.h"

#include <Preferences.h>
#include <esp_timer.h>
#include <math.h>

// Conversion aproximada de cuentas a mV (atenuacion 11 dB: ~0..3100 mV)
constexpr float MV_PER_COUNT = 3100.0f / 4095.0f;

constexpr uint32_t RMS_N = EMG_FS_HZ * EMG_RMS_WINDOW_MS / 1000;  // 200 muestras
constexpr uint32_t MIN_ON_SAMPLES = EMG_FS_HZ * EMG_MIN_ON_MS / 1000;
constexpr uint32_t MIN_OFF_SAMPLES = EMG_FS_HZ * EMG_MIN_OFF_MS / 1000;
constexpr uint32_t SAT_HOLD_SAMPLES = EMG_FS_HZ / 2;
constexpr int SAT_LOW = 8;
constexpr int SAT_HIGH = 4087;

// ---- Estado compartido entre el timer (core 0) y loop() (core 1) -----------
static portMUX_TYPE emgMux = portMUX_INITIALIZER_UNLOCKED;
static EmgSnapshot shared = {};
static float peakMv = 0;
static volatile float thresholdMv = EMG_DEFAULT_THRESHOLD_MV;
static float savedMvcMv = 0;  // maximo de la ultima calibracion OK (MVC)

// ---- Estado interno del procesamiento (solo lo toca el timer) ---------------
static float dcMean = -1;
static float sq[RMS_N];
static double sqSum = 0;
static uint32_t sqIdx = 0;
static uint32_t aboveCount = 0;
static uint32_t belowCount = 0;
static bool activeState = false;
static uint32_t pendingOnsetMs = 0;
static uint32_t satHold = 0;
static uint32_t nSamples = 0;

static Preferences prefs;

static void emgSampleCb(void *) {
  int raw = analogRead(EMG_PIN);
  float x = raw * MV_PER_COUNT;

  // Pasa-altas digital: elimina el offset DC del circuito (sea cual sea)
  if (dcMean < 0) dcMean = x;
  dcMean += (x - dcMean) * (1.0f / (EMG_FS_HZ * EMG_DC_TAU_S));
  float ac = x - dcMean;

  // RMS en ventana deslizante: sqrt(media de x^2). |x| y x^2 son la rectificacion.
  float s = ac * ac;
  sqSum += s - sq[sqIdx];
  sq[sqIdx] = s;
  if (++sqIdx >= RMS_N) {
    sqIdx = 0;
    // recalcular la suma de vez en cuando para que no acumule error
    double t = 0;
    for (uint32_t i = 0; i < RMS_N; i++) t += sq[i];
    sqSum = t;
  }
  float rms = sqrtf((float)(sqSum > 0 ? sqSum / RMS_N : 0));

  // Deteccion con histeresis y duracion minima
  float thrOn = thresholdMv;
  float thrOff = thrOn * EMG_RELEASE_RATIO;
  uint32_t now = millis();
  if (!activeState) {
    if (rms > thrOn) {
      if (aboveCount == 0) pendingOnsetMs = now;
      if (++aboveCount >= MIN_ON_SAMPLES) {
        activeState = true;
        belowCount = 0;
      }
    } else {
      aboveCount = 0;
    }
  } else {
    if (rms < thrOff) {
      if (++belowCount >= MIN_OFF_SAMPLES) {
        activeState = false;
        aboveCount = 0;
      }
    } else {
      belowCount = 0;
    }
  }

  if (raw <= SAT_LOW || raw >= SAT_HIGH) satHold = SAT_HOLD_SAMPLES;
  else if (satHold > 0) satHold--;
  nSamples++;

  portENTER_CRITICAL(&emgMux);
  shared.rmsMv = rms;
  shared.rawMv = ac;
  shared.dcMv = dcMean;
  shared.rawCounts = raw;
  shared.active = activeState;
  shared.saturated = satHold > 0;
  shared.onsetMs = pendingOnsetMs;
  shared.sampleCount = nSamples;
  if (rms > peakMv) peakMv = rms;
  portEXIT_CRITICAL(&emgMux);
}

static esp_timer_handle_t sampleTimer = nullptr;

void emgBegin() {
  prefs.begin("emg", false);
  thresholdMv = prefs.getFloat("thr", EMG_DEFAULT_THRESHOLD_MV);
  savedMvcMv = prefs.getFloat("mvc", 0);

  analogReadResolution(12);
  analogSetPinAttenuation(EMG_PIN, ADC_11db);
  analogRead(EMG_PIN);  // inicializa el canal antes de arrancar el timer

  esp_timer_create_args_t args = {};
  args.callback = emgSampleCb;
  args.name = "emg";
  args.dispatch_method = ESP_TIMER_TASK;
  esp_timer_create(&args, &sampleTimer);
  esp_timer_start_periodic(sampleTimer, 1000000ULL / EMG_FS_HZ);

  Serial.printf("[EMG] pin GPIO%d, %lu Hz, umbral %.1f mV. Comandos: p=plot, c=calibrar, t<mV>=umbral, ?=ayuda\n",
                EMG_PIN, (unsigned long)EMG_FS_HZ, (double)thresholdMv);
}

EmgSnapshot emgRead() {
  portENTER_CRITICAL(&emgMux);
  EmgSnapshot s = shared;
  portEXIT_CRITICAL(&emgMux);
  return s;
}

float emgThreshold() { return thresholdMv; }

void emgSetThreshold(float mv) {
  if (mv < 1.0f) mv = 1.0f;
  if (mv > 3000.0f) mv = 3000.0f;
  thresholdMv = mv;
  prefs.putFloat("thr", mv);
}

float emgTakePeakMv() {
  portENTER_CRITICAL(&emgMux);
  float p = peakMv;
  peakMv = 0;
  portEXIT_CRITICAL(&emgMux);
  return p;
}

// ---- Calibracion -------------------------------------------------------------
constexpr uint32_t CAL_REST_MS = 3000;
constexpr uint32_t CAL_SQUEEZE_MS = 3000;
constexpr uint32_t CAL_RESULT_MS = 2500;  // cuanto se muestra el resultado

static EmgCalState calState = CAL_IDLE;
static uint32_t calStart = 0;
static double calRestSum = 0;
static uint32_t calRestN = 0;
static float calRest = 0;
static float calMax = 0;

void emgStartCalibration() {
  calState = CAL_REST;
  calStart = millis();
  calRestSum = 0;
  calRestN = 0;
  calMax = 0;
  Serial.println("[CAL] Reposo: mano relajada 3 s...");
}

void emgCancelCalibration() {
  if (calState != CAL_IDLE) {
    calState = CAL_IDLE;
    Serial.println("[CAL] Cancelada");
  }
}

EmgCalState emgCalibrationUpdate() {
  if (calState == CAL_IDLE) return calState;
  uint32_t el = millis() - calStart;
  float rms = emgRead().rmsMv;

  switch (calState) {
    case CAL_REST:
      // se ignoran los primeros 500 ms (la persona se acomoda)
      if (el > 500) {
        calRestSum += rms;
        calRestN++;
      }
      if (el >= CAL_REST_MS) {
        calRest = calRestN ? (float)(calRestSum / calRestN) : rms;
        calState = CAL_SQUEEZE;
        calStart = millis();
        Serial.printf("[CAL] Reposo = %.1f mV. Ahora: apretar fuerte 3 s...\n", (double)calRest);
      }
      break;
    case CAL_SQUEEZE:
      if (rms > calMax) calMax = rms;
      if (el >= CAL_SQUEEZE_MS) {
        // Debe haber una diferencia clara entre reposo y contraccion
        if (calMax > calRest * 2.0f && calMax - calRest > 5.0f) {
          float thr = calRest + EMG_CAL_FRACTION * (calMax - calRest);
          emgSetThreshold(thr);
          savedMvcMv = calMax;
          prefs.putFloat("mvc", calMax);
          calState = CAL_DONE;
          Serial.printf("[CAL] OK: reposo %.1f mV, maximo %.1f mV -> umbral %.1f mV\n",
                        (double)calRest, (double)calMax, (double)thr);
        } else {
          calState = CAL_FAILED;
          Serial.printf("[CAL] FALLO: reposo %.1f mV, maximo %.1f mV (sin diferencia clara). Revisar electrodos.\n",
                        (double)calRest, (double)calMax);
        }
        calStart = millis();
      }
      break;
    case CAL_DONE:
    case CAL_FAILED:
      if (el >= CAL_RESULT_MS) calState = CAL_IDLE;
      break;
    default:
      break;
  }
  return calState;
}

EmgCalState emgCalibrationState() { return calState; }

uint32_t emgCalibrationRemainingMs() {
  uint32_t el = millis() - calStart;
  uint32_t total = calState == CAL_REST ? CAL_REST_MS : calState == CAL_SQUEEZE ? CAL_SQUEEZE_MS : CAL_RESULT_MS;
  return el >= total ? 0 : total - el;
}

float emgCalRestMv() { return calRest; }
float emgCalMaxMv() { return calMax; }
float emgMvcMv() { return savedMvcMv; }

// ---- Serial ------------------------------------------------------------------
static bool plotOn = false;

void emgSerialUpdate() {
  // Comandos por el Serial Monitor
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'p' || c == 'P') {
      plotOn = !plotOn;
      Serial.printf("[EMG] plot %s\n", plotOn ? "ON" : "OFF");
    } else if (c == 'c' || c == 'C') {
      emgStartCalibration();
    } else if (c == 't' || c == 'T') {
      float v = Serial.parseFloat();
      if (v > 0) {
        emgSetThreshold(v);
        Serial.printf("[EMG] umbral = %.1f mV\n", (double)emgThreshold());
      }
    } else if (c == '?') {
      EmgSnapshot s = emgRead();
      Serial.printf("[EMG] pin GPIO%d raw=%d rms=%.1f mV umbral=%.1f mV activo=%d saturado=%d muestras=%lu\n",
                    EMG_PIN, s.rawCounts, (double)s.rmsMv, (double)emgThreshold(), s.active, s.saturated,
                    (unsigned long)s.sampleCount);
      Serial.println("[EMG] p = plot on/off | c = calibrar | t120 = umbral 120 mV | ? = estado");
    }
  }

  // Serial Plotter cada 20 ms (50 Hz)
  static uint32_t last = 0;
  if (plotOn && millis() - last >= 20) {
    last = millis();
    EmgSnapshot s = emgRead();
    Serial.printf("emg:%.1f,rms:%.1f,umbral:%.1f,activo:%d\n", (double)s.rawMv, (double)s.rmsMv,
                  (double)emgThreshold(), s.active ? (int)(emgThreshold() * 1.5f) : 0);
  }
}
