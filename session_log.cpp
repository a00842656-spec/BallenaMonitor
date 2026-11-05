// Ver session_log.h
#include "session_log.h"

#include <WebServer.h>
#include <WiFi.h>
#include <sys/time.h>
#include <time.h>

#include "emg.h"
#include "motor_protocol.h"

// ---- Red Wi-Fi propia de la pantalla -------------------------------------------
static const char *WIFI_AP_SSID = "Ballena-EMG";
static const char *WIFI_AP_PASS = "ballena123";   // minimo 8 caracteres

// ---- Capacidad ----------------------------------------------------------------
constexpr uint32_t ENV_PERIOD_MS = 50;                       // envolvente a 20 Hz
constexpr uint32_t MAX_ENV_POINTS = 20UL * 60 * 30;          // 30 min a 20 Hz
constexpr uint32_t MAX_REPS = 300;
constexpr uint32_t MAX_PAUSES = 100;

struct RepRec {
  uint32_t tMs;      // desde GRABAR
  uint32_t durMs;
  float peakMv;
  int32_t latencyMs;
  bool manual;
};
struct PauseRec {
  uint32_t tMs;
  uint32_t durMs;
};

static WebServer server(80);

static uint16_t *env = nullptr;      // RMS en decimas de mV
static uint32_t envMax = 0;
static uint32_t envN = 0;
static bool envTruncated = false;
static RepRec reps[MAX_REPS];
static uint32_t repN = 0;
static PauseRec pauses[MAX_PAUSES];
static uint32_t pauseN = 0;
static bool pausedNow = false;

static bool recording = false;
static uint32_t startMs = 0;
static uint32_t lastEnvMs = 0;
static uint32_t sessionId = 0;

// Reloj: la pantalla no tiene RTC ni internet; la app le manda la hora con /sync
static bool clockValid = false;
static char recPaciente[16] = "SIN_ID";
static char recFecha[12] = "";   // "2026-11-05"
static char recHora[10] = "";    // "10:32:05"

// Para la app (nombres que usa el equipo en FlutterFlow): EMG cada 100 ms, max 400
constexpr uint32_t APP_SAMPLE_INTERVAL_MS = 100;
constexpr uint32_t APP_MAX_MUESTRAS = 400;

// Datos de la sesion publicada (se llenan al ENVIAR)
static bool ready = false;
static uint32_t sentDurMs = 0;
static int sentObjetivo = 0;
static int sentReps = 0;
static bool sentCompleted = false;
static float sentThreshold = 0;
static float sentMvc = 0;
static int sentSesionPaciente = 0;

// ---- HTTP ---------------------------------------------------------------------
static void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
}

static void handleSync() {
  cors();
  if (!server.hasArg("epoch")) {
    server.send(400, "application/json", "{\"error\":\"falta epoch (segundos Unix UTC)\"}");
    return;
  }
  long long epoch = atoll(server.arg("epoch").c_str());
  if (epoch > 100000000000LL) epoch /= 1000;  // si mandan milisegundos
  int tzMin = server.hasArg("tz_min") ? server.arg("tz_min").toInt() : -360;  // Mexico: UTC-6
  struct timeval tv = {(time_t)epoch, 0};
  settimeofday(&tv, nullptr);
  // POSIX TZ: el signo va al reves ("UTC6" = 6 h detras de UTC)
  char tz[16];
  snprintf(tz, sizeof(tz), "UTC%+d", -tzMin / 60);
  setenv("TZ", tz, 1);
  tzset();
  clockValid = true;
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char b[96];
  strftime(b, sizeof(b), "{\"ok\":true,\"hora_local\":\"%Y-%m-%d %H:%M:%S\"}", &lt);
  server.send(200, "application/json", b);
  Serial.printf("[RELOJ] Sincronizado por la app: %s\n", b);
}

static void handleEstado() {
  cors();
  char b[260];
  snprintf(b, sizeof(b),
           "{\"grabando\":%s,\"listo\":%s,\"id\":%lu,\"segundos_grabados\":%.1f,"
           "\"pacienteID\":\"%s\",\"hora_sincronizada\":%s}",
           recording ? "true" : "false", ready ? "true" : "false", (unsigned long)sessionId,
           recording ? (millis() - startMs) / 1000.0 : sentDurMs / 1000.0, recPaciente,
           clockValid ? "true" : "false");
  server.send(200, "application/json", b);
}

static void handleJson() {
  cors();
  if (!ready) {
    server.send(404, "application/json", "{\"error\":\"sin datos: presiona GRABAR y luego ENVIAR\"}");
    return;
  }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  char b[256];
  // Promedios solo de las repeticiones detectadas por EMG (las manuales no tienen senal)
  float meanDur = 0, meanPeak = 0;
  uint32_t nEmg = 0;
  for (uint32_t i = 0; i < repN; i++) {
    if (reps[i].manual) continue;
    meanDur += reps[i].durMs;
    meanPeak += reps[i].peakMv;
    nEmg++;
  }
  if (nEmg) {
    meanDur /= nEmg;
    meanPeak /= nEmg;
  }
  float logroPct = sentObjetivo > 0 ? 100.0f * sentReps / sentObjetivo : 0;
  if (logroPct > 100) logroPct = 100;
  snprintf(b, sizeof(b),
           "{\"id\":%lu,\"pacienteID\":\"%s\",\"sesion_paciente\":%d,\"sesionID\":\"%s #%d\","
           "\"fecha\":\"%s\",\"hora\":\"%s\",\"hora_valida\":%s,\"modo\":\"REHABILITACION\",",
           (unsigned long)sessionId, recPaciente, sentSesionPaciente, recPaciente, sentSesionPaciente,
           recFecha, recHora, clockValid && recFecha[0] ? "true" : "false");
  server.sendContent(b);
  snprintf(b, sizeof(b),
           "\"duracion_s\":%.1f,\"objetivo\":%d,\"repeticiones\":%d,\"completada\":%s,\"umbral_mV\":%.1f,",
           sentDurMs / 1000.0, sentObjetivo, sentReps, sentCompleted ? "true" : "false", (double)sentThreshold);
  server.sendContent(b);
  snprintf(b, sizeof(b),
           "\"repeticiones_emg\":%lu,\"repeticiones_manual\":%lu,\"logro_pct\":%.1f,"
           "\"activacion_media_ms\":%.0f,\"pico_medio_mV\":%.1f,\"pico_medio_uV\":%.1f,",
           (unsigned long)nEmg, (unsigned long)(repN - nEmg), (double)logroPct, (double)meanDur,
           (double)meanPeak, (double)(meanPeak * 1000.0f / EMG_TOTAL_GAIN));
  server.sendContent(b);
  if (sentMvc > 0 && nEmg) {
    snprintf(b, sizeof(b), "\"mvc_mV\":%.1f,\"activacion_media_pct_mvc\":%.1f,", (double)sentMvc,
             (double)(100.0f * meanPeak / sentMvc));
  } else {
    snprintf(b, sizeof(b), "\"mvc_mV\":null,\"activacion_media_pct_mvc\":null,");
  }
  server.sendContent(b);
  snprintf(b, sizeof(b), "\"ganancia_total\":%.1f,\"envolvente_hz\":%lu,\"envolvente_truncada\":%s,",
           (double)EMG_TOTAL_GAIN, (unsigned long)(1000 / ENV_PERIOD_MS), envTruncated ? "true" : "false");
  server.sendContent(b);

  server.sendContent("\"repeticiones_detalle\":[");
  for (uint32_t i = 0; i < repN; i++) {
    snprintf(b, sizeof(b),
             "%s{\"n\":%lu,\"fuente\":\"%s\",\"t_s\":%.2f,\"duracion_ms\":%lu,\"pico_mV\":%.1f,\"latencia_ms\":%ld}",
             i ? "," : "", (unsigned long)(i + 1), reps[i].manual ? "MANUAL" : "EMG", reps[i].tMs / 1000.0,
             (unsigned long)reps[i].durMs, (double)reps[i].peakMv, (long)reps[i].latencyMs);
    server.sendContent(b);
  }
  server.sendContent("],\"pausas\":[");
  for (uint32_t i = 0; i < pauseN; i++) {
    snprintf(b, sizeof(b), "%s{\"t_s\":%.2f,\"duracion_s\":%.1f}", i ? "," : "", pauses[i].tMs / 1000.0,
             pauses[i].durMs / 1000.0);
    server.sendContent(b);
  }
  // lecturasEMG: mismo formato que el codigo del equipo (int, cada 100 ms, max 400).
  // Si la sesion es mas larga, se reparten 400 muestras en toda la sesion.
  {
    uint32_t step = APP_SAMPLE_INTERVAL_MS / ENV_PERIOD_MS;  // 2
    uint32_t avail = envN / step;
    uint32_t n = avail < APP_MAX_MUESTRAS ? avail : APP_MAX_MUESTRAS;
    float stride = n ? (float)avail / n : 1;
    uint32_t intervalMs = (uint32_t)(APP_SAMPLE_INTERVAL_MS * stride);
    snprintf(b, sizeof(b), "],\"muestrasGrabadas\":%lu,\"SAMPLE_INTERVAL_ms\":%lu,\"lecturasEMG\":[",
             (unsigned long)n, (unsigned long)intervalMs);
    server.sendContent(b);
    String c;
    c.reserve(1100);
    for (uint32_t i = 0; i < n; i++) {
      uint32_t idx = (uint32_t)(i * stride) * step;
      char v[12];
      snprintf(v, sizeof(v), "%s%d", i ? "," : "", (int)lroundf(env[idx] / 10.0f));
      c += v;
      if (c.length() > 1000) {
        server.sendContent(c);
        c = "";
      }
    }
    server.sendContent(c);
  }
  server.sendContent("],\"envolvente_mV\":[");
  // en bloques para no gastar memoria
  String chunk;
  chunk.reserve(1100);
  for (uint32_t i = 0; i < envN; i++) {
    char v[12];
    snprintf(v, sizeof(v), "%s%.1f", i ? "," : "", env[i] / 10.0);
    chunk += v;
    if (chunk.length() > 1000) {
      server.sendContent(chunk);
      chunk = "";
    }
  }
  chunk += "]}";
  server.sendContent(chunk);
  server.sendContent("");  // fin
}

static void handleCsv() {
  cors();
  if (!ready) {
    server.send(404, "text/plain", "sin datos");
    return;
  }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent("t_s,rms_mV\n");
  String chunk;
  chunk.reserve(1100);
  for (uint32_t i = 0; i < envN; i++) {
    char v[32];
    snprintf(v, sizeof(v), "%.2f,%.1f\n", i * ENV_PERIOD_MS / 1000.0, env[i] / 10.0);
    chunk += v;
    if (chunk.length() > 1000) {
      server.sendContent(chunk);
      chunk = "";
    }
  }
  server.sendContent(chunk);
  server.sendContent("");
}

static void handleRoot() {
  cors();
  char b[700];
  snprintf(b, sizeof(b),
           "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
           "<title>Ballena EMG</title><body style='font-family:sans-serif'>"
           "<h2>Ballena &mdash; sesion de rehabilitacion</h2>"
           "<p>Estado: %s</p><p>Sesion #%lu (%s %s %s): %d / %d repeticiones, %.0f s</p>"
           "<p><a href='/sesion.json'>sesion.json</a> &middot; <a href='/sesion.csv'>sesion.csv</a> "
           "&middot; <a href='/estado'>estado</a></p>",
           recording ? "grabando..." : ready ? "datos listos" : "sin datos",
           (unsigned long)sessionId, recPaciente, recFecha, recHora, sentReps, sentObjetivo, sentDurMs / 1000.0);
  server.send(200, "text/html", b);
}

// ---- API ------------------------------------------------------------------------
void sessionLogBegin() {
  // Memoria para la envolvente: PSRAM si hay, si no un poco menos en RAM interna
  envMax = MAX_ENV_POINTS;
  env = (uint16_t *)ps_malloc(envMax * sizeof(uint16_t));
  if (env == nullptr) {
    envMax = 20UL * 60 * 10;  // 10 min
    env = (uint16_t *)malloc(envMax * sizeof(uint16_t));
  }
  if (env == nullptr) envMax = 0;

  // Red propia en el MISMO canal que ESP-NOW (si no, la tortuga deja de recibir)
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS, ESPNOW_CHANNEL);
  server.on("/", handleRoot);
  server.on("/estado", handleEstado);
  server.on("/sync", handleSync);
  server.on("/sesion.json", handleJson);
  server.on("/sesion.csv", handleCsv);
  server.begin();
  Serial.printf("[WIFI] Red '%s' (contrasena '%s'), http://%s  canal %u\n", WIFI_AP_SSID, WIFI_AP_PASS,
                WiFi.softAPIP().toString().c_str(), ESPNOW_CHANNEL);
}

void sessionLogLoop() {
  server.handleClient();

  if (recording && millis() - lastEnvMs >= ENV_PERIOD_MS) {
    lastEnvMs += ENV_PERIOD_MS;
    if (envN < envMax) {
      float r = emgRead().rmsMv * 10.0f;
      env[envN++] = (uint16_t)constrain(r, 0.0f, 65535.0f);
    } else {
      envTruncated = true;
    }
  }
}

void sessionLogStart(int objetivo, const char *pacienteId) {
  recording = true;
  strncpy(recPaciente, pacienteId ? pacienteId : "SIN_ID", sizeof(recPaciente) - 1);
  recPaciente[sizeof(recPaciente) - 1] = 0;
  recFecha[0] = recHora[0] = 0;
  if (clockValid) {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    strftime(recFecha, sizeof(recFecha), "%Y-%m-%d", &lt);
    strftime(recHora, sizeof(recHora), "%H:%M:%S", &lt);
  }
  ready = false;   // lo anterior se descarta
  startMs = millis();
  lastEnvMs = startMs;
  envN = 0;
  envTruncated = false;
  repN = 0;
  pauseN = 0;
  pausedNow = false;
  Serial.printf("[GRABAR] Inicio: %s %s %s (objetivo %d)\n", recPaciente, recFecha[0] ? recFecha : "(sin fecha: falta /sync)",
                recHora, objetivo);
}

void sessionLogRep(uint32_t onsetMs, uint32_t durMs, float peakMv, int32_t latencyMs, bool manual) {
  if (!recording || repN >= MAX_REPS) return;
  uint32_t t = onsetMs >= startMs ? onsetMs - startMs : 0;
  reps[repN++] = {t, durMs, peakMv, latencyMs, manual};
}

void sessionLogPause(bool paused) {
  if (!recording || paused == pausedNow) return;
  pausedNow = paused;
  if (paused) {
    if (pauseN < MAX_PAUSES) pauses[pauseN++] = {millis() - startMs, 0};
  } else if (pauseN > 0) {
    pauses[pauseN - 1].durMs = millis() - startMs - pauses[pauseN - 1].tMs;
  }
}

bool sessionLogSend(int objetivo, int repeticiones, bool completada, int sesionPaciente) {
  if (!recording) return false;
  sessionLogPause(false);
  recording = false;
  sentDurMs = millis() - startMs;
  sentObjetivo = objetivo;
  sentReps = repeticiones;
  sentCompleted = completada;
  sentThreshold = emgThreshold();
  sentMvc = emgMvcMv();
  sentSesionPaciente = sesionPaciente;
  sessionId++;
  ready = true;
  Serial.printf("[ENVIAR] %s #%d (envio %lu): %d/%d reps, %.1f s, %lu puntos -> http://%s/sesion.json\n",
                recPaciente, sesionPaciente, (unsigned long)sessionId, repeticiones, objetivo, sentDurMs / 1000.0, (unsigned long)envN,
                WiFi.softAPIP().toString().c_str());
  return true;
}

bool sessionLogRecording() { return recording; }
bool sessionLogClockValid() { return clockValid; }
uint32_t sessionLogElapsedMs() { return recording ? millis() - startMs : 0; }
uint32_t sessionLogId() { return sessionId; }
const char *sessionLogApSsid() { return WIFI_AP_SSID; }
