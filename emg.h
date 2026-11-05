// sEMG: adquisicion digital, envolvente RMS y deteccion de activacion.
//
// Cadena (ver Stage 2):
//   salida del filtro analogico -> ADC1 del ESP32-S3 (2 kHz, 12 bits)
//   -> quitar offset DC (pasa-altas digital ~0.3 Hz)
//   -> rectificacion |x| y RMS en ventana de 100 ms
//   -> comparacion con umbral (con histeresis y duracion minima)
//
// El muestreo corre en un timer del core 0 (no depende de que la pantalla
// tarde en dibujar). Desde loop() solo se leen los resultados.

#pragma once

#include <Arduino.h>

// ---- Configuracion de hardware --------------------------------------------
// PIN del ADC donde entra la salida del circuito sEMG.
// IMPORTANTE: con ESP-NOW (Wi-Fi) encendido solo funcionan los pines de ADC1
// (GPIO1..GPIO10 en el ESP32-S3). En el conector de expansion de la
// WT32-SC01 Plus el unico de ADC1 es GPIO10. GPIO11..14 son ADC2: NO usarlos.
constexpr int EMG_PIN = 10;

// Ganancia analogica total (AD620 x postamplificacion). Solo se usa para
// reportar la amplitud estimada en el electrodo (uV) en el log de pruebas.
constexpr float EMG_TOTAL_GAIN = 10.0f;

// ---- Procesamiento --------------------------------------------------------
constexpr uint32_t EMG_FS_HZ = 2000;          // frecuencia de muestreo
constexpr uint32_t EMG_RMS_WINDOW_MS = 100;   // ventana de la envolvente RMS
constexpr float EMG_DC_TAU_S = 0.5f;          // constante del pasa-altas digital

// ---- Deteccion ------------------------------------------------------------
constexpr float EMG_DEFAULT_THRESHOLD_MV = 60.0f;  // umbral inicial (RMS, mV en el ADC)
constexpr float EMG_RELEASE_RATIO = 0.7f;   // se apaga por debajo de umbral x 0.7 (histeresis)
constexpr uint32_t EMG_MIN_ON_MS = 150;     // debe superar el umbral este tiempo para activar (rechaza golpes/espigas cortas)
constexpr uint32_t EMG_MIN_OFF_MS = 150;    // debe bajar del umbral de apagado este tiempo para desactivar
// Calibracion: umbral = reposo + EMG_CAL_FRACTION x (maximo - reposo)
constexpr float EMG_CAL_FRACTION = 0.30f;

// Escala de la barra en pantalla (mV RMS que llenan la barra al 100 %)
constexpr float EMG_DISPLAY_FULL_MV = 400.0f;

struct EmgSnapshot {
  float rmsMv;        // envolvente RMS actual (mV en la entrada del ADC)
  float rawMv;        // ultima muestra sin offset (mV)
  float dcMv;         // nivel DC (offset) de la entrada, mV. ~0 o ~3100 = cable suelto / saturado
  int rawCounts;      // ultima muestra cruda (0..4095)
  bool active;        // activacion detectada (con histeresis)
  bool saturated;     // la senal toco 0 V o 3.1 V en los ultimos ~0.5 s
  uint32_t onsetMs;   // millis() del primer cruce del umbral de la activacion actual
  uint32_t sampleCount;
};

void emgBegin();
EmgSnapshot emgRead();

float emgThreshold();
void emgSetThreshold(float mv);  // se guarda en memoria (sobrevive al reinicio)

// Pico de RMS desde la ultima llamada (para el log de cada activacion)
float emgTakePeakMv();

// ---- Calibracion (no bloqueante) ------------------------------------------
enum EmgCalState : uint8_t { CAL_IDLE, CAL_REST, CAL_SQUEEZE, CAL_DONE, CAL_FAILED };
void emgStartCalibration();
void emgCancelCalibration();
EmgCalState emgCalibrationUpdate();  // llamar UNA vez por loop(); devuelve el estado actual
EmgCalState emgCalibrationState();   // solo consulta (no avanza)
uint32_t emgCalibrationRemainingMs();
float emgCalRestMv();
float emgCalMaxMv();
// Maximo de la ultima calibracion exitosa (se guarda en memoria). 0 = nunca calibrado.
// Sirve como referencia de contraccion maxima (MVC) para expresar la activacion en %.
float emgMvcMv();

// ---- Salida serial ----------------------------------------------------------
// Serial Plotter: "emg:..,rms:..,thr:..,act:.." cada 20 ms (activar con 'p')
void emgSerialUpdate();
