// sEMG: adquisicion digital, envolvente RMS y deteccion de activacion.
//
// Cadena (la misma del codigo "EMG facial v2" del equipo, ahora dentro de la pantalla):
//   salida del circuito analogico -> ADC1 del ESP32-S3, 1 kHz, sobremuestreo x4 (mV calibrados)
//   -> notch 60 Hz -> pasa-altas 20 Hz -> pasa-bajas 300 Hz   (biquads digitales)
//   -> rechazo de artefactos (|y| > 1 V se toma como 0)
//   -> RMS en ventana de 150 ms -> suavizado EMA (alpha 0.02)
//   -> umbral con histeresis (70 %) y tiempo minimo de 200 ms; 1 s de calentamiento
//
// El muestreo corre en un timer del core 0 (no depende de que la pantalla
// tarde en dibujar). Desde loop() solo se leen los resultados.

#pragma once

#include <Arduino.h>

// ---- De donde viene la senal ---------------------------------------------------
// true  = la senal la lee OTRO ESP32 (pin D34) y la manda por ESP-NOW (sketch
//         "EMG_Emisor"). La pantalla NO usa su ADC. (configuracion actual)
// false = el circuito va directo al GPIO10 de la pantalla (conector Extended IO, pin 3).
constexpr bool EMG_REMOTE = true;
// Sin mensajes del ESP32 EMG durante este tiempo -> senal = 0 y "EMG sin conexion"
// (la tortuga se detiene: falla segura)
constexpr uint32_t EMG_REMOTE_TIMEOUT_MS = 300;

// ---- Configuracion de hardware (solo si EMG_REMOTE = false) -----------------
// PIN del ADC donde entra la salida del circuito sEMG.
// IMPORTANTE: con ESP-NOW (Wi-Fi) encendido solo funcionan los pines de ADC1
// (GPIO1..GPIO10 en el ESP32-S3). En el conector de expansion de la
// WT32-SC01 Plus el unico de ADC1 es GPIO10. GPIO11..14 son ADC2: NO usarlos.
constexpr int EMG_PIN = 10;

// Ganancia analogica total (AD620 x postamplificacion). Solo se usa para
// reportar la amplitud estimada en el electrodo (uV) en el log de pruebas.
constexpr float EMG_TOTAL_GAIN = 10.0f;

// ---- Procesamiento --------------------------------------------------------
constexpr uint32_t EMG_FS_HZ = 1000;          // frecuencia de muestreo (los filtros estan calculados para 1 kHz)
constexpr int EMG_OVERSAMPLE = 4;             // lecturas promediadas por muestra
constexpr uint32_t EMG_RMS_WINDOW_MS = 150;   // ventana de la envolvente RMS
constexpr float EMG_ALPHA_RMS = 0.02f;        // suavizado del RMS (mas chico = mas suave)
constexpr float EMG_ARTIFACT_MV = 1000.0f;    // |senal filtrada| mayor que esto = artefacto -> 0
constexpr uint32_t EMG_WARMUP_MS = 1000;      // los filtros se estabilizan, sin deteccion
constexpr float EMG_DC_TAU_S = 0.5f;          // solo para medir el nivel DC (estado de conexion)

// ---- Deteccion ------------------------------------------------------------
constexpr float EMG_DEFAULT_THRESHOLD_MV = 30.0f;  // umbral inicial = UMBRAL_MIN del codigo del equipo (mV RMS)
constexpr float EMG_RELEASE_RATIO = 0.7f;   // se apaga por debajo de umbral x 0.7 (histeresis)
constexpr uint32_t EMG_MIN_ON_MS = 200;     // debe superar el umbral este tiempo para activar (TIEMPO_MIN)
constexpr uint32_t EMG_MIN_OFF_MS = 200;    // debe bajar del umbral de apagado este tiempo para desactivar
// Calibracion: umbral = reposo + EMG_CAL_FRACTION x (maximo - reposo)
constexpr float EMG_CAL_FRACTION = 0.30f;

// Escala de la barra en pantalla (mV RMS que llenan la barra al 100 %)
constexpr float EMG_DISPLAY_FULL_MV = 400.0f;

struct EmgSnapshot {
  float rmsMv;        // envolvente RMS actual (mV en la entrada del ADC)
  float rawMv;        // ultima muestra sin offset (mV)
  float dcMv;         // nivel DC (offset) de la entrada, mV. ~0 o ~3100 = cable suelto / saturado
  int rawCounts;      // ultima entrada del ADC en mV (0..~3100)
  bool active;        // activacion detectada (con histeresis)
  bool saturated;     // la senal toco 0 V o 3.1 V en los ultimos ~0.5 s
  uint32_t onsetMs;   // millis() del primer cruce del umbral de la activacion actual
  uint32_t sampleCount;
};

void emgBegin();
EmgSnapshot emgRead();

// ---- Modo remoto (EMG_REMOTE = true) -------------------------------------------
// Lo llama el callback de ESP-NOW al recibir un EmgMsg (ver motor_protocol.h).
void emgRemoteFeed(float rmsMv, float peakMv, float inputMv, bool saturated, bool warming);
bool emgRemoteLinkOk();          // llegan mensajes del ESP32 EMG
uint32_t emgRemoteLost();        // paquetes perdidos (segun seq)
void emgRemoteCountSeq(uint8_t seq);

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
// Serial Plotter cada 20 ms (activar con 'p'), mismo formato que el codigo del equipo:
//   RMS_mV, Pico_mV, Estado (0/200), Para activar (umbral), Max:350, Min:20
void emgSerialUpdate();
