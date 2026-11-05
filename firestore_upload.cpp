// Ver firestore_upload.h
#include "firestore_upload.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include <time.h>

#include "motor_protocol.h"
#include "session_log.h"
#include <lvgl.h>

constexpr uint32_t WIFI_TIMEOUT_UPLOAD_MS = 15000;
constexpr uint32_t WIFI_TIMEOUT_BOOT_MS = 8000;
constexpr uint32_t NTP_TIMEOUT_MS = 6000;
constexpr int TZ_MIN_MEXICO = -360;   // UTC-6 (fecha/hora local en la pantalla)

enum Job : uint8_t { JOB_CLOCK, JOB_UPLOAD };

static volatile FirestoreState state = FS_IDLE;
static volatile bool busy = false;
static volatile uint32_t resultId = 0;
static char message[96] = "";
static String payload;            // JSON listo, con "%FECHA%" en lugar de la fecha
static uint32_t payloadGrabarMs = 0;
static bool unsent = false;

static void setMsg(const char *m) {
  strncpy(message, m, sizeof(message) - 1);
  message[sizeof(message) - 1] = 0;
}

static bool wifiConnect(uint32_t timeoutMs) {
  WiFi.begin(FIRESTORE_WIFI_SSID, FIRESTORE_WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return true;
}

// Regresa al canal de ESP-NOW para que la tortuga y el ESP32 EMG vuelvan a funcionar
static void wifiRelease() {
  WiFi.disconnect(false, false);
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

// Hora por internet. Al final deja la zona horaria de Mexico para la pantalla.
static bool ntpSync() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t t0 = millis();
  while (time(nullptr) < 1600000000) {
    if (millis() - t0 > NTP_TIMEOUT_MS) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  sessionLogSetClock((long long)time(nullptr), TZ_MIN_MEXICO, nullptr, 0);
  return true;
}

static void task(void *arg) {
  Job job = (Job)(uintptr_t)arg;

  if (job == JOB_CLOCK) {
    if (wifiConnect(WIFI_TIMEOUT_BOOT_MS)) {
      bool ok = ntpSync();
      Serial.printf("[NUBE] Hora al encender: %s\n", ok ? "OK" : "sin respuesta NTP");
    } else {
      Serial.printf("[NUBE] Hotspot '%s' no encontrado al encender (la hora se toma al ENVIAR)\n",
                    FIRESTORE_WIFI_SSID);
    }
    wifiRelease();
    busy = false;
    vTaskDelete(nullptr);
    return;
  }

  // ---- JOB_UPLOAD ----
  Serial.printf("[NUBE] Conectando a '%s'...\n", FIRESTORE_WIFI_SSID);
  if (!wifiConnect(WIFI_TIMEOUT_UPLOAD_MS)) {
    wifiRelease();
    unsent = true;
    setMsg(LV_SYMBOL_WARNING " Sin Wi-Fi (hotspot)\nENVIAR otra vez = reintentar");
    Serial.println("[NUBE] ERROR: no se pudo conectar al hotspot");
    state = FS_FAIL;
    resultId = resultId + 1;
    busy = false;
    vTaskDelete(nullptr);
    return;
  }

  bool timeOk = ntpSync() || sessionLogClockValid();
  String body = payload;
  if (timeOk) {
    // fecha = momento de GRABAR (ahora - lo que ha pasado desde GRABAR), en UTC
    time_t start = time(nullptr) - (time_t)((millis() - payloadGrabarMs) / 1000);
    struct tm g;
    gmtime_r(&start, &g);
    char f[32];
    strftime(f, sizeof(f), "%Y-%m-%dT%H:%M:%SZ", &g);
    body.replace("%FECHA%", f);
  } else {
    body.replace("{\"timestampValue\": \"%FECHA%\"}", "{\"nullValue\": null}");
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(12000);
  String url = "https://firestore.googleapis.com/v1/projects/" FIRESTORE_PROJECT_ID
               "/databases/(default)/documents/" FIRESTORE_COLLECTION;
  int code = -1;
  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
    if (code != 200 && code != 201) {
      Serial.printf("[NUBE] Respuesta: %s\n", http.getString().c_str());
    }
    http.end();
  }
  wifiRelease();

  if (code == 200 || code == 201) {
    unsent = false;
    setMsg(LV_SYMBOL_OK " Enviado a la app");
    Serial.printf("[NUBE] Envio exitoso (%u bytes)\n", (unsigned)body.length());
    state = FS_OK;
  } else {
    unsent = true;
    char b[96];
    snprintf(b, sizeof(b), LV_SYMBOL_WARNING " Error al enviar (%d)\nENVIAR otra vez = reintentar", code);
    setMsg(b);
    Serial.printf("[NUBE] ERROR HTTP %d\n", code);
    state = FS_FAIL;
  }
  resultId = resultId + 1;
  busy = false;
  vTaskDelete(nullptr);
}

static bool startJob(Job job) {
  if (busy) return false;
  busy = true;
  if (job == JOB_UPLOAD) {
    state = FS_BUSY;
    setMsg("Enviando a la app...");
  }
  if (xTaskCreatePinnedToCore(task, "nube", 12288, (void *)(uintptr_t)job, 1, nullptr, 0) != pdPASS) {
    busy = false;
    if (job == JOB_UPLOAD) {
      state = FS_FAIL;
      unsent = true;
      setMsg(LV_SYMBOL_WARNING " Error interno al enviar");
      resultId = resultId + 1;
    }
    return false;
  }
  return true;
}

void firestoreBegin() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  if (FIRESTORE_SYNC_CLOCK_AT_BOOT) startJob(JOB_CLOCK);
}

static void addField(String &p, const char *name, const String &value) {
  p += ", \"";
  p += name;
  p += "\": ";
  p += value;
}

static String intV(long v) { return "{\"integerValue\": \"" + String(v) + "\"}"; }
static String dblV(double v) { return "{\"doubleValue\": " + String(v, 1) + "}"; }

bool firestoreStartUpload(const char *pacienteId, int sesionPaciente, uint32_t grabarMillis) {
  if (busy) return false;
  static int onda[400];
  SessionSummary s;
  if (!sessionLogSummary(s, onda, 400)) return false;

  // Mismo formato que uploadToFirestore() del equipo + campos extra para el reporte
  String p;
  p.reserve(12000);
  p = "{\"fields\": {";
  p += "\"pacienteID\": {\"stringValue\": \"";
  p += pacienteId;
  p += "\"}, \"fecha\": {\"timestampValue\": \"%FECHA%\"}, \"onda\": {\"arrayValue\": {";
  if (s.nOnda) {
    p += "\"values\": [";
    for (uint32_t i = 0; i < s.nOnda; i++) {
      if (i) p += ",";
      p += "{\"integerValue\": \"";
      p += onda[i];
      p += "\"}";
    }
    p += "]";
  }
  p += "}}";
  char sid[40];
  snprintf(sid, sizeof(sid), "{\"stringValue\": \"%s #%d\"}", pacienteId, sesionPaciente);
  addField(p, "sesionID", sid);
  addField(p, "sesion_paciente", intV(sesionPaciente));
  addField(p, "duracion_s", dblV(s.durMs / 1000.0));
  addField(p, "objetivo", intV(s.objetivo));
  addField(p, "repeticiones", intV(s.repeticiones));
  addField(p, "repeticiones_emg", intV(s.repsEmg));
  addField(p, "completada", s.completada ? "{\"booleanValue\": true}" : "{\"booleanValue\": false}");
  addField(p, "intervalo_ms", intV(s.intervaloMs));
  addField(p, "umbral_mV", dblV(s.umbralMv));
  addField(p, "pico_medio_uV", dblV(s.picoMedioUv));
  addField(p, "activacion_media_pct_mvc", s.pctMvc >= 0 ? dblV(s.pctMvc) : String("{\"nullValue\": null}"));
  p += "}}";

  payload = p;
  payloadGrabarMs = grabarMillis;
  unsent = true;   // hasta que termine bien
  Serial.printf("[NUBE] Sesion %s #%d lista: %lu muestras cada %lu ms\n", pacienteId, sesionPaciente,
                (unsigned long)s.nOnda, (unsigned long)s.intervaloMs);
  return startJob(JOB_UPLOAD);
}

bool firestoreRetry() {
  if (!unsent || payload.length() == 0) return false;
  return startJob(JOB_UPLOAD);
}

FirestoreState firestoreState() { return state; }
bool firestoreBusy() { return busy; }
bool firestoreHasUnsent() { return unsent && payload.length() > 0; }
const char *firestoreMessage() { return message; }
uint32_t firestoreResultId() { return resultId; }
