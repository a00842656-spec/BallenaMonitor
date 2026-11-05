// Ver ble_app.h
#include "ble_app.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "session_log.h"

#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

constexpr uint32_t CHUNK_GAP_MS = 12;        // pausa entre pedazos (el telefono no pierde datos)
constexpr uint32_t AUTO_SEND_DELAY_MS = 1500; // al conectar: tiempo para que la app se suscriba

static BLEServer *server = nullptr;
static BLECharacteristic *txChar = nullptr;
static volatile bool connected = false;
static volatile bool justConnected = false;
static volatile bool justDisconnected = false;
static uint32_t connectedAtMs = 0;

// Comando recibido (lo escribe la tarea BLE, lo procesa loop())
static portMUX_TYPE cmdMux = portMUX_INITIALIZER_UNLOCKED;
static char cmdBuf[96];
static volatile bool cmdPending = false;

// Mensaje que se esta enviando por pedazos
static String outMsg;
static size_t outPos = 0;
static uint32_t lastChunkMs = 0;
static String queued;             // siguiente mensaje (si llega otro mientras se envia)

static bool sessionPending = false;  // ENVIAR sin app conectada -> mandar al conectar

class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    connected = true;
    justConnected = true;
  }
  void onDisconnect(BLEServer *) override {
    connected = false;
    justDisconnected = true;
  }
};

class RxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String v = c->getValue();
    portENTER_CRITICAL(&cmdMux);
    strncpy(cmdBuf, v.c_str(), sizeof(cmdBuf) - 1);
    cmdBuf[sizeof(cmdBuf) - 1] = 0;
    cmdPending = true;
    portEXIT_CRITICAL(&cmdMux);
  }
};

static void sendLine(const String &json) {
  String line = json;
  line += '\n';
  if (outPos < outMsg.length()) {
    queued += line;  // se manda despues del actual
  } else {
    outMsg = line;
    outPos = 0;
  }
}

static bool buildSession(String &out) {
  String j;
  if (!sessionLogBuildJson(j, false)) return false;
  // {"id":..}  ->  {"tipo":"sesion","id":..}
  out = "{\"tipo\":\"sesion\",";
  out += j.substring(1);
  return true;
}

void bleAppBegin() {
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEDevice::setMTU(247);
  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCb());

  BLEService *svc = server->createService(NUS_SERVICE_UUID);
  txChar = svc->createCharacteristic(NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
#if !CONFIG_BT_NIMBLE_ENABLED
  txChar->addDescriptor(new BLE2902());
#endif
  BLECharacteristic *rx = svc->createCharacteristic(
      NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCb());
  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("[BLE] Anunciando '%s' (servicio Nordic UART). Esperando la app...\n", BLE_DEVICE_NAME);
}

bool bleAppConnected() { return connected; }
bool bleAppSending() { return outPos < outMsg.length(); }

void bleAppSendEstado() {
  if (!connected) return;
  char b[300];
  sessionLogEstadoJson(b, sizeof(b));
  String s = "{\"tipo\":\"estado\",";
  s += (b + 1);
  sendLine(s);
}

bool bleAppSendSession() {
  if (!connected) {
    sessionPending = true;
    Serial.println("[BLE] App no conectada: la sesion se mandara al conectarse");
    return false;
  }
  String s;
  if (!buildSession(s)) return false;
  sendLine(s);
  sessionPending = false;
  Serial.printf("[BLE] Enviando sesion (%u bytes)\n", (unsigned)s.length());
  return true;
}

static void handleCommand(const char *cmd) {
  while (*cmd == ' ') cmd++;
  Serial.printf("[BLE] Comando: %s\n", cmd);
  if (strncasecmp(cmd, "SYNC", 4) == 0) {
    long long epoch = 0;
    int tz = -360;
    const char *p = cmd + 4;
    epoch = atoll(p);
    while (*p == ' ') p++;
    while (*p && *p != ' ') p++;  // saltar el epoch
    if (*p == ' ') tz = atoi(p);
    char b[120];
    bool ok = sessionLogSetClock(epoch, tz, b, sizeof(b));
    String s = "{\"tipo\":\"reloj\",";
    s += (b + 1);
    sendLine(s);
    (void)ok;
  } else if (strncasecmp(cmd, "ESTADO", 6) == 0 || cmd[0] == '?') {
    bleAppSendEstado();
  } else if (strncasecmp(cmd, "GET", 3) == 0) {
    String s;
    if (buildSession(s)) sendLine(s);
    else sendLine("{\"tipo\":\"error\",\"error\":\"sin datos: presiona GRABAR y luego ENVIAR\"}");
  } else {
    sendLine("{\"tipo\":\"error\",\"error\":\"comando desconocido (SYNC, ESTADO, GET)\"}");
  }
}

void bleAppLoop() {
  if (server == nullptr) return;

  if (justDisconnected) {
    justDisconnected = false;
    outMsg = "";
    outPos = 0;
    queued = "";
    Serial.println("[BLE] App desconectada");
    delay(50);
    BLEDevice::startAdvertising();  // para que se pueda volver a conectar
  }
  if (justConnected) {
    justConnected = false;
    connectedAtMs = millis();
    Serial.println("[BLE] App conectada");
    char b[300];
    sessionLogEstadoJson(b, sizeof(b));
    String s = "{\"tipo\":\"hola\",\"dispositivo\":\"";
    s += BLE_DEVICE_NAME;
    s += "\",";
    s += (b + 1);
    sendLine(s);
  }
  if (connected && sessionPending && millis() - connectedAtMs > AUTO_SEND_DELAY_MS) {
    bleAppSendSession();
  }

  if (cmdPending) {
    char c[sizeof(cmdBuf)];
    portENTER_CRITICAL(&cmdMux);
    memcpy(c, cmdBuf, sizeof(c));
    cmdPending = false;
    portEXIT_CRITICAL(&cmdMux);
    // quitar \r \n del final
    for (int i = strlen(c) - 1; i >= 0 && (c[i] == '\r' || c[i] == '\n' || c[i] == ' '); i--) c[i] = 0;
    handleCommand(c);
  }

  // Mandar el siguiente pedazo
  if (!connected) return;
  if (outPos >= outMsg.length()) {
    if (queued.length() == 0) return;
    outMsg = queued;
    queued = "";
    outPos = 0;
  }
  if (millis() - lastChunkMs < CHUNK_GAP_MS) return;
  if (millis() - connectedAtMs < 300) return;  // dar tiempo a que la app se suscriba
  lastChunkMs = millis();
  uint16_t mtu = server->getPeerMTU(server->getConnId());
  size_t chunk = (mtu > 23 ? mtu - 3 : 20);
  if (chunk > 180) chunk = 180;
  size_t n = outMsg.length() - outPos;
  if (n > chunk) n = chunk;
  txChar->setValue((uint8_t *)outMsg.c_str() + outPos, n);
  txChar->notify();
  outPos += n;
  if (outPos >= outMsg.length()) {
    outMsg = "";
    outPos = 0;
  }
}
