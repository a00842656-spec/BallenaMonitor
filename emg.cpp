// Ver emg.h
#include "emg.h"

#include <Preferences.h>
#include <esp_timer.h>
#include <math.h>

constexpr uint32_t RMS_N = EMG_FS_HZ * EMG_RMS_WINDOW_MS / 1000;  // 150 muestras
constexpr uint32_t MIN_ON_SAMPLES = EMG_FS_HZ * EMG_MIN_ON_MS / 1000;
constexpr uint32_t MIN_OFF_SAMPLES = EMG_FS_HZ * EMG_MIN_OFF_MS / 1000;
constexpr uint32_t WARMUP_SAMPLES = EMG_FS_HZ * EMG_WARMUP_MS / 1000;
constexpr uint32_t SAT_HOLD_SAMPLES = EMG_FS_HZ / 2;
constexpr float SAT_LOW_MV = 20.0f;     // entrada pegada a 0 V
constexpr float SAT_HIGH_MV = 3050.0f;  // entrada pegada al maximo del ADC (~3.1 V)

// ---- Biquad (igual que el codigo del equipo) ----------------------------------
struct Biquad {
  float b0, b1, b2, a1, a2;
  float x1, x2, y1, y2;
  float process(float x) {
    float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x; y2 = y1; y1 = y;
    return y;
  }
};
// fs = 1000 Hz
static Biquad notch60 = {0.97618f, -1.81526f, 0.97618f, -1.80379f, 0.94090f, 0, 0, 0, 0};  // notch 60 Hz, r = 0.97
static Biquad hp20    = {0.91497f, -1.82994f, 0.91497f, -1.82272f, 0.83718f, 0, 0, 0, 0};  // pasa-altas 20 Hz
static Biquad lp300   = {0.39134f,  0.78268f, 0.39134f,  0.36953f, 0.19582f, 0, 0, 0, 0};  // pasa-bajas 300 Hz

// ---- Estado compartido entre el timer (core 0) y loop() (core 1) -----------
static portMUX_TYPE emgMux = portMUX_INITIALIZER_UNLOCKED;
static EmgSnapshot shared = {};
static float peakMv = 0;                 // pico de RMS (para el log de cada activacion)
static volatile float blockPeakMv = 0;   // pico |senal filtrada| (Pico_mV del plotter)
static volatile float thresholdMv = EMG_DEFAULT_THRESHOLD_MV;
static float savedMvcMv = 0;  // maximo de la ultima calibracion OK (MVC)

// ---- Estado interno del procesamiento (solo lo toca el timer) ---------------
static float dcMean = -1;
static float sq[RMS_N];
static double sqSum = 0;
static uint32_t sqIdx = 0;
static float rmsSuave = 0;
static uint32_t aboveCount = 0;
static uint32_t belowCount = 0;
static bool activeState = false;
static uint32_t pendingOnsetMs = 0;
static uint32_t satHold = 0;
static uint32_t nSamples = 0;

static Preferences prefs;

static void emgSampleCb(void *) {
  // 0) Sobremuestreo x4 (mV ya calibrados por el ESP32)
  uint32_t acc = 0;
  for (int i = 0; i < EMG_OVERSAMPLE; i++) acc += analogReadMilliVolts(EMG_PIN);
  float v = (float)acc / EMG_OVERSAMPLE;  // mV

  // nivel DC solo para saber si hay circuito conectado (no se usa para detectar)
  if (dcMean < 0) dcMean = v;
  dcMean += (v - dcMean) * (1.0f / (EMG_FS_HZ * EMG_DC_TAU_S));

  // 1) Filtros: notch 60 Hz -> pasa-altas 20 Hz -> pasa-bajas 300 Hz
  float y = lp300.process(hp20.process(notch60.process(v)));

  // 2) Rechazo de artefactos
  float ym = (fabsf(y) > EMG_ARTIFACT_MV) ? 0 : y;

  // 3) RMS en ventana de 150 ms
  float s = ym * ym;
  sqSum += s - sq[sqIdx];
  sq[sqIdx] = s;
  if (++sqIdx >= RMS_N) {
    sqIdx = 0;
    double t = 0;  // recalcular para que no acumule error
    for (uint32_t i = 0; i < RMS_N; i++) t += sq[i];
    sqSum = t;
  }
  float rms = sqrtf((float)(sqSum > 0 ? sqSum / RMS_N : 0));

  // 4) Suavizado de la envolvente
  rmsSuave += EMG_ALPHA_RMS * (rms - rmsSuave);

  if (fabsf(ym) > blockPeakMv) blockPeakMv = fabsf(ym);
  nSamples++;

  // 5) Estado con histeresis y tiempo minimo (despues del calentamiento)
  float thrOn = thresholdMv;
  float thrOff = thrOn * EMG_RELEASE_RATIO;
  uint32_t now = millis();
  if (nSamples > WARMUP_SAMPLES) {
    if (!activeState) {
      if (rmsSuave > thrOn) {
        if (aboveCount == 0) pendingOnsetMs = now;
        if (++aboveCount >= MIN_ON_SAMPLES) {
          activeState = true;
          belowCount = 0;
        }
      } else {
        aboveCount = 0;
      }
    } else {
      if (rmsSuave < thrOff) {
        if (++belowCount >= MIN_OFF_SAMPLES) {
          activeState = false;
          aboveCount = 0;
        }
      } else {
        belowCount = 0;
      }
    }
  }

  if (v <= SAT_LOW_MV || v >= SAT_HIGH_MV) satHold = SAT_HOLD_SAMPLES;
  else if (satHold > 0) satHold--;

  portENTER_CRITICAL(&emgMux);
  shared.rmsMv = rmsSuave;
  shared.rawMv = y;
  shared.dcMv = dcMean;
  shared.rawCounts = (int)v;   // ahora en mV (antes cuentas)
  shared.active = activeState;
  shared.saturated = satHold > 0;
  shared.onsetMs = pendingOnsetMs;
  shared.sampleCount = nSamples;
  if (rmsSuave > peakMv) peakMv = rmsSuave;
  portEXIT_CRITICAL(&emgMux);
}

// ---- Modo remoto: la envolvente llega del ESP32 EMG por ESP-NOW ----------------
static volatile uint32_t lastRemoteMs = 0;
static uint32_t remoteAboveSince = 0;   // millis() del primer mensaje sobre el umbral
static uint32_t remoteBelowSince = 0;
static int lastSeq = -1;
static volatile uint32_t lostPackets = 0;

void emgRemoteCountSeq(uint8_t seq) {
  if (lastSeq >= 0) {
    uint8_t gap = (uint8_t)(seq - (uint8_t)lastSeq);
    if (gap > 1 && gap < 100) lostPackets = lostPackets + (gap - 1);
  }
  lastSeq = seq;
}

void emgRemoteFeed(float rmsMv, float pkMv, float inputMv, bool saturated, bool warming) {
  uint32_t now = millis();
  float thrOn = thresholdMv;
  float thrOff = thrOn * EMG_RELEASE_RATIO;

  portENTER_CRITICAL(&emgMux);
  if (warming) {
    activeState = false;
    remoteAboveSince = remoteBelowSince = 0;
  } else if (!activeState) {
    if (rmsMv > thrOn) {
      if (remoteAboveSince == 0) {
        remoteAboveSince = now;
        pendingOnsetMs = now;
      }
      if (now - remoteAboveSince >= EMG_MIN_ON_MS) {
        activeState = true;
        remoteBelowSince = 0;
      }
    } else {
      remoteAboveSince = 0;
    }
  } else {
    if (rmsMv < thrOff) {
      if (remoteBelowSince == 0) remoteBelowSince = now;
      if (now - remoteBelowSince >= EMG_MIN_OFF_MS) {
        activeState = false;
        remoteAboveSince = 0;
      }
    } else {
      remoteBelowSince = 0;
    }
  }
  nSamples++;
  shared.rmsMv = rmsMv;
  shared.rawMv = pkMv;
  shared.dcMv = inputMv;
  shared.rawCounts = (int)inputMv;
  shared.active = activeState;
  shared.saturated = saturated;
  shared.onsetMs = pendingOnsetMs;
  shared.sampleCount = nSamples;
  if (rmsMv > peakMv) peakMv = rmsMv;
  if (pkMv > blockPeakMv) blockPeakMv = pkMv;
  lastRemoteMs = now;
  portEXIT_CRITICAL(&emgMux);
}

bool emgRemoteLinkOk() {
  uint32_t t = lastRemoteMs;
  return t != 0 && millis() - t < EMG_REMOTE_TIMEOUT_MS;
}

uint32_t emgRemoteLost() { return lostPackets; }

static esp_timer_handle_t sampleTimer = nullptr;

void emgBegin() {
  prefs.begin("emg", false);
  // "thr2": umbral de la cadena nueva (el de la version anterior no es comparable)
  thresholdMv = prefs.getFloat("thr2", EMG_DEFAULT_THRESHOLD_MV);
  savedMvcMv = prefs.getFloat("mvc2", 0);

  if (EMG_REMOTE) {
    // La senal la manda el ESP32 EMG por ESP-NOW (ver motor_protocol.h / EmgMsg)
    Serial.printf("[EMG] Modo REMOTO: esperando al ESP32 EMG por ESP-NOW. Umbral %.1f mV. "
                  "Comandos: p=plot, c=calibrar, t<mV>=umbral, ?=estado\n", (double)thresholdMv);
    return;
  }

  analogReadResolution(12);
  analogSetPinAttenuation(EMG_PIN, ADC_11db);
  analogReadMilliVolts(EMG_PIN);  // inicializa el canal y su calibracion antes del timer

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
  if (EMG_REMOTE && !(lastRemoteMs != 0 && millis() - lastRemoteMs < EMG_REMOTE_TIMEOUT_MS)) {
    // Sin datos del ESP32 EMG: senal en cero y sin activacion (la tortuga se detiene)
    activeState = false;
    remoteAboveSince = remoteBelowSince = 0;
    shared.rmsMv = 0;
    shared.rawMv = 0;
    shared.active = false;
    shared.saturated = false;
  }
  EmgSnapshot s = shared;
  portEXIT_CRITICAL(&emgMux);
  return s;
}

float emgThreshold() { return thresholdMv; }

static uint32_t thrSaveAtMs = 0;

static void saveThresholdIfDue() {
  if (thrSaveAtMs != 0 && (int32_t)(millis() - thrSaveAtMs) >= 0) {
    thrSaveAtMs = 0;
    prefs.putFloat("thr2", thresholdMv);
  }
}

void emgSetThreshold(float mv) {
  if (mv < 1.0f) mv = 1.0f;
  if (mv > 3000.0f) mv = 3000.0f;
  thresholdMv = mv;
  thrSaveAtMs = millis() + 2000;   // guardar 2 s despues del ultimo cambio (no en cada toque de +/-)
  if (thrSaveAtMs == 0) thrSaveAtMs = 1;
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
          prefs.putFloat("mvc2", calMax);
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
  saveThresholdIfDue();
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
      if (EMG_REMOTE) {
        Serial.printf("[EMG] REMOTO: enlace %s, mensajes %lu, perdidos %lu\n", emgRemoteLinkOk() ? "OK" : "SIN DATOS",
                      (unsigned long)s.sampleCount, (unsigned long)emgRemoteLost());
      }
      Serial.printf("[EMG] pin GPIO%d entrada=%d mV rms=%.1f mV umbral=%.1f mV activo=%d saturado=%d muestras=%lu\n",
                    EMG_PIN, s.rawCounts, (double)s.rmsMv, (double)emgThreshold(), s.active, s.saturated,
                    (unsigned long)s.sampleCount);
      Serial.println("[EMG] p = plot on/off | c = calibrar | t120 = umbral 120 mV | ? = estado");
    }
  }

  // Serial Plotter cada 20 ms (50 Hz), mismo formato que el codigo del equipo
  static uint32_t last = 0;
  if (plotOn && millis() - last >= 20) {
    last = millis();
    EmgSnapshot s = emgRead();
    float pk = blockPeakMv;
    blockPeakMv = 0;
    Serial.printf("RMS_mV:%.1f,Pico_mV:%.1f,Estado:%d,Para activar:%.0f,Max:350,Min:20\n", (double)s.rmsMv,
                  (double)pk, s.active ? 200 : 0, (double)emgThreshold());
  }
}
