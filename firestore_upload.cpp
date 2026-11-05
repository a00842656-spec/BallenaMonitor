// Ver firestore_upload.h
#include "firestore_upload.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <esp_wifi.h>
#include <time.h>

#include "motor_protocol.h"
#include "session_log.h"
#include <lvgl.h>

constexpr uint32_t WIFI_TIMEOUT_UPLOAD_MS = 12000;
constexpr bool FECHA_LOCAL_CON_Z = true;
#define FIRESTORE_HOST "firestore.googleapis.com"
constexpr uint32_t WIFI_TIMEOUT_BOOT_MS = 8000;
constexpr uint32_t NTP_TIMEOUT_MS = 6000;
constexpr int TZ_MIN_MEXICO = -360;   // UTC-6 (fecha/hora local en la pantalla)

enum Job : uint8_t { JOB_CLOCK, JOB_UPLOAD };

static volatile FirestoreState state = FS_IDLE;
static volatile bool busy = false;
static volatile uint32_t resultId = 0;
static char message[160] = "";
static String payload;            // JSON listo, con "%FECHA%" en lugar de la fecha
static uint32_t payloadGrabarMs = 0;
static bool unsent = false;

static void setMsg(const char *m) {
  strncpy(message, m, sizeof(message) - 1);
  message[sizeof(message) - 1] = 0;
}

static char ssidUsed[33] = FIRESTORE_WIFI_SSID;
static char failReason[64] = "";

static bool tryConnect(const char *ssid, uint32_t timeoutMs) {
  WiFi.begin(ssid, FIRESTORE_WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > timeoutMs) {
      WiFi.disconnect(false, false);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return true;
}

static String trimLower(const char *x) {
  String t(x);
  t.trim();
  t.toLowerCase();
  return t;
}

// Conecta al hotspot. Si falla, busca las redes y explica por que (Serial + pantalla).
// Si existe una red con el mismo nombre pero con otros espacios/mayusculas, usa esa.
static bool wifiConnect(uint32_t timeoutMs) {
  failReason[0] = 0;
  if (tryConnect(ssidUsed, timeoutMs)) return true;

  Serial.printf("[NUBE] No conecto a '%s' (estado %d). Buscando redes...\n", ssidUsed, (int)WiFi.status());
  int n = WiFi.scanNetworks(false, true);
  String want = trimLower(FIRESTORE_WIFI_SSID);
  int found = -1;
  for (int i = 0; i < n; i++) {
    Serial.printf("[NUBE]   red '%s'  canal %d  senal %d dBm\n", WiFi.SSID(i).c_str(), (int)WiFi.channel(i),
                  (int)WiFi.RSSI(i));
    if (found < 0 && trimLower(WiFi.SSID(i).c_str()) == want) found = i;
  }
  if (found < 0) {
    snprintf(failReason, sizeof(failReason), "No se ve la red '%s'", FIRESTORE_WIFI_SSID);
    Serial.println("[NUBE] La red no aparece: hotspot apagado, en 5 GHz, o nombre distinto");
    WiFi.scanDelete();
    return false;
  }
  String exact = WiFi.SSID(found);
  WiFi.scanDelete();
  if (exact != ssidUsed) {
    Serial.printf("[NUBE] El nombre real es '%s': reintentando con ese\n", exact.c_str());
    strncpy(ssidUsed, exact.c_str(), sizeof(ssidUsed) - 1);
    if (tryConnect(ssidUsed, timeoutMs)) return true;
  }
  snprintf(failReason, sizeof(failReason), "Red '%s' visible:\nrevisar contrasena", ssidUsed);
  Serial.println("[NUBE] La red existe pero no acepta la conexion: contrasena incorrecta?");
  return false;
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

// ---- DNS dentro del hilo de red (lwIP) ------------------------------------------
// Network.hostByName() del core 3.3.x se cae ("Required to lock TCPIP core
// functionality") cuando se llama desde otra tarea. Por eso la direccion de
// Firestore se busca aqui, dentro del hilo de lwIP, y se conecta por IP.
struct DnsReq {
  const char *host;
  ip_addr_t addr;
  SemaphoreHandle_t done;
  volatile bool ok;
};
static DnsReq dnsReq;   // estatico: el callback puede llegar tarde

static void dnsFound(const char *, const ip_addr_t *ip, void *arg) {
  DnsReq *r = (DnsReq *)arg;
  if (ip != nullptr) {
    r->addr = *ip;
    r->ok = true;
  }
  xSemaphoreGive(r->done);
}

static void dnsStart(void *arg) {
  DnsReq *r = (DnsReq *)arg;
  err_t e = dns_gethostbyname_addrtype(r->host, &r->addr, dnsFound, r, LWIP_DNS_ADDRTYPE_IPV4);
  if (e == ERR_OK) {
    r->ok = true;
    xSemaphoreGive(r->done);
  } else if (e != ERR_INPROGRESS) {
    xSemaphoreGive(r->done);
  }
}

static bool resolveHost(const char *host, IPAddress &out) {
  if (dnsReq.done == nullptr) dnsReq.done = xSemaphoreCreateBinary();
  xSemaphoreTake(dnsReq.done, 0);  // limpiar
  dnsReq.host = host;
  dnsReq.ok = false;
  if (tcpip_callback(dnsStart, &dnsReq) != ERR_OK) return false;
  if (xSemaphoreTake(dnsReq.done, pdMS_TO_TICKS(8000)) != pdTRUE || !dnsReq.ok) return false;
  out = IPAddress(ip_2_ip4(&dnsReq.addr)->addr);
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
    char b[160];
    snprintf(b, sizeof(b), LV_SYMBOL_WARNING " Sin Wi-Fi: %s\nENVIAR otra vez = reintentar",
             failReason[0] ? failReason : "hotspot");
    setMsg(b);
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
    // Igual que el codigo del equipo: hora LOCAL de Mexico escrita con "Z"
    // (asi la app la muestra con la hora correcta). false = UTC real.
    if (FECHA_LOCAL_CON_Z) localtime_r(&start, &g);
    else gmtime_r(&start, &g);
    char f[32];
    strftime(f, sizeof(f), "%Y-%m-%dT%H:%M:%SZ", &g);
    body.replace("%FECHA%", f);
  } else {
    body.replace("{\"timestampValue\": \"%FECHA%\"}", "{\"nullValue\": null}");
  }

  // HTTPS necesita ~40 KB de RAM interna libre (mbedTLS). Se intenta hasta 3 veces.
  String url = "https://" FIRESTORE_HOST "/v1/projects/" FIRESTORE_PROJECT_ID
               "/databases/(default)/documents/" FIRESTORE_COLLECTION;
  int code = -1;
  for (int intento = 1; intento <= 3 && code != 200 && code != 201; intento++) {
    Serial.printf("[NUBE] Intento %d: RAM interna libre %u, bloque mayor %u\n", intento,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15);            // segundos
    client.setHandshakeTimeout(20);   // segundos
    IPAddress ip;
    if (!resolveHost(FIRESTORE_HOST, ip)) {
      Serial.println("[NUBE] DNS: no se encontro firestore.googleapis.com");
      code = -100;
    } else if (!client.connect(ip, 443, FIRESTORE_HOST, nullptr, nullptr, nullptr)) {
      char err[100] = "";
      client.lastError(err, sizeof(err));
      Serial.printf("[NUBE] TLS no conecto a %s: %s\n", ip.toString().c_str(), err);
      code = -101;
    } else {
      // HTTPClient reutiliza la conexion ya abierta (no vuelve a buscar el nombre)
      HTTPClient http;
      http.setReuse(true);
      http.setTimeout(15000);
      if (http.begin(client, url)) {
        http.addHeader("Content-Type", "application/json");
        code = http.POST(body);
        if (code != 200 && code != 201) {
          Serial.printf("[NUBE] HTTP %d (%s)\n", code, http.errorToString(code).c_str());
          if (code > 0) Serial.printf("[NUBE] Respuesta: %s\n", http.getString().c_str());
        }
        http.end();
      }
    }
    client.stop();
    if (code > 0 && code != 200 && code != 201) break;   // respuesta del servidor: no sirve reintentar
    if (code != 200 && code != 201) vTaskDelay(pdMS_TO_TICKS(1500));
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
