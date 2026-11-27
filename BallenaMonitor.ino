#include <Arduino.h>
#include <lvgl.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "ui.h"
#include <esp_now.h> // ESP-NOW 라이브러리
#include <WiFi.h>    // WiFi 라이브러리
#include <esp_wifi.h>
#include "motor_protocol.h" // Tortuga XIAO와 동일한 파일 (tortuga-motor-test/src)
#include "audio.h"           // musica y efectos (conector SPEAKER)
#include "emg.h"             // adquisicion sEMG + deteccion de activacion
#include "emg_panel.h"       // barra / grafica EMG en pantalla
#include "session_log.h"     // GRABAR / ENVIAR (datos de la sesion)
#include "ble_app.h"         // (opcional) Bluetooth con la aplicacion
#include "firestore_upload.h" // ENVIAR -> Firestore (la base de datos de la app)

// La app (FlutterFlow) lee Firestore: ENVIAR sube la sesion por el hotspot (firestore_upload.h).
// true = ademas anunciar Bluetooth (ble_app.h). No hace falta para la app actual.
constexpr bool USAR_BLUETOOTH_APP = false;
#include <Preferences.h>
bool isGamePaused = false;

// MAC del XIAO de la tortuga (la imprime su Serial: "XIAO MAC: ...").
// Con FF:FF:FF:FF:FF:FF (broadcast) tambien funciona, sin conocer la MAC
// (pero entonces la pantalla no puede saber si la tortuga recibio el mensaje).
uint8_t turtleAddress[] = {0xE4, 0xB3, 0x23, 0xB6, 0x0A, 0x80};

esp_now_peer_info_t peerInfo;

// Velocidad de la tortuga (duty PWM 0..255)
constexpr uint8_t TURTLE_SPEED = 180;   // MODO REHABILITACION, REGRESAR, 화살표: 고정 속도

// ---- MODO JUEGO: 근육 활성도(RMS)에 비례하는 속도 ----
// RMS 가 임계값(umbral)일 때 JUEGO_SPEED_MIN, "최대 힘" 이상일 때 JUEGO_SPEED_MAX.
// 최대 힘 = CAL 로 측정한 최대 수축(MVC). CAL 을 안 했으면 umbral x JUEGO_SIN_CAL_FACTOR.
//   - JUEGO_SPEED_MIN: 거북이가 겨우 움직이는 값보다 조금 크게 (너무 작으면 모터가 안 돎)
//   - JUEGO_MVC_FRACCION: MVC 의 몇 % 에서 최고 속도에 도달할지 (아이가 최대로 힘주지 않아도 되게 70 %)
constexpr bool JUEGO_VELOCIDAD_VARIABLE = true;   // false = 예전처럼 고정 속도
constexpr uint8_t JUEGO_SPEED_MIN = 110;
constexpr uint8_t JUEGO_SPEED_MAX = 255;
constexpr float JUEGO_MVC_FRACCION = 0.70f;
constexpr float JUEGO_SIN_CAL_FACTOR = 4.0f;
constexpr uint8_t JUEGO_SPEED_STEP = 10;          // 이만큼 이상 바뀌면 바로 새 속도 전송

// REGRESAR: cuanto tiempo retrocede la tortuga (ms)
constexpr uint32_t REGRESAR_MS = 1000;

// ---- Paro de emergencia (boton fisico en la ballena) ----
// ESTOP_PIN: GPIO del boton (el otro lado del boton va a GND; se usa pull-up interno).
//   -1 = no hay boton conectado.
//   Sugerido: GPIO21 o GPIO11 del conector de expansion (libres; para entrada digital
//   no importa que sean ADC2).
// ESTOP_ACTIVE_LEVEL: nivel del pin cuando el boton esta PRESIONADO.
//   - Contacto NC (normalmente cerrado, lo recomendado): HIGH
//     (si se corta el cable tambien se detiene = falla segura)
//   - Contacto NO (normalmente abierto): LOW
// Al presionarlo: la tortuga se detiene, la EMG se ignora y aparece un aviso rojo.
// El aviso se quita tocandolo, solo cuando el boton ya se solto.
constexpr int ESTOP_PIN = -1;
constexpr int ESTOP_ACTIVE_LEVEL = HIGH;

// ---- Bateria de la tortuga (la manda la tortuga, ver motor_protocol.h) ----
constexpr uint32_t BATTERY_TIMEOUT_MS = 5000;  // sin datos en 5 s -> se muestra "--"
constexpr uint8_t BATTERY_LOW_PCT = 20;        // por debajo: barra en rojo

// true = imprimir por Serial cuando dibujar la pantalla tarda mas de 20 ms
// (en false para que no ensucie el Serial Plotter de la EMG)
constexpr bool DEBUG_TIMING = false;

// ---- Estado del enlace ESP-NOW (lo llena el callback de envio) ----
static volatile uint32_t lastAckMs = 0;   // ultimo envio confirmado por la tortuga
static volatile uint32_t sendFailCount = 0;
constexpr uint32_t LINK_TIMEOUT_MS = 2000;

static void onEspNowSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        lastAckMs = millis();
    } else {
        sendFailCount = sendFailCount + 1;
    }
}

// ---- Recepcion: bateria de la tortuga ----
static volatile uint32_t lastTelemetryMs = 0;
static volatile uint16_t turtleBatteryMv = 0;
static volatile uint8_t turtleBatteryPct = 0;

// ---- Bateria de la ballena (pila de 9 V de la pantalla) ----
// Se mide en la misma pantalla con un divisor de voltaje:
//   + de la pila (despues del boton rojo = IN+ del regulador) -> 100k -> EXT_IO1 -> 30k -> GND
//   EXT_IO1 = GPIO10 = pin 3 del conector "Extended IO" (cable amarillo del arnes)
// BALLENA_BAT_PIN = -1: no se mide aqui; la manda el ESP32 de la EMG (BAT_PIN de EMG_Emisor).
// Sin pila (solo USB) o sin divisor la lectura sale fuera de rango y se muestra "--".
constexpr int BALLENA_BAT_PIN = 10;
constexpr float BALLENA_BAT_DIVIDER = (100.0f + 30.0f) / 30.0f;   // (R arriba + R abajo) / R abajo
constexpr float BALLENA_BAT_V_FULL = 9.0f;    // pila nueva
constexpr float BALLENA_BAT_V_EMPTY = 7.0f;   // abajo de esto el regulador ya no da 5 V estables

static volatile uint32_t lastBallenaBatMs = 0;
static volatile uint16_t ballenaBatteryMv = 0;
static volatile uint8_t ballenaBatteryPct = 0;

static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (info == NULL || data == NULL) return;
    // EMG del ESP32 del circuito analogico (de cualquier MAC)
    if (len == (int)sizeof(EmgMsg) && data[0] == EMG_MSG_MAGIC) {
        EmgMsg m;
        memcpy(&m, data, sizeof(m));
        emgRemoteCountSeq(m.seq);
        emgRemoteFeed(m.rms_x10 / 10.0f, m.peak_x10 / 10.0f, (float)m.input_mv, (m.flags & 1) != 0,
                      (m.flags & 2) != 0);
        return;
    }
    // 고래 배터리 (EMG ESP32 가 보냄, 어느 MAC 이든 받음)
    if (len == (int)sizeof(BallenaBatMsg) && data[0] == BALLENA_BAT_MAGIC) {
        if (BALLENA_BAT_PIN >= 0) return;   // se mide en la pantalla: ignorar la del ESP32 EMG
        BallenaBatMsg m;
        memcpy(&m, data, sizeof(m));
        ballenaBatteryMv = m.battery_mv;
        ballenaBatteryPct = m.battery_pct > 100 ? 100 : m.battery_pct;
        lastBallenaBatMs = millis();
        return;
    }
    if (memcmp(info->src_addr, turtleAddress, 6) != 0 &&
        !(turtleAddress[0] == 0xFF && turtleAddress[1] == 0xFF)) {
        return;  // no es nuestra tortuga
    }
    if (len == (int)sizeof(TelemetryMsg) && data[0] == TELEMETRY_MSG_MAGIC) {
        TelemetryMsg m;
        memcpy(&m, data, sizeof(m));
        turtleBatteryMv = m.battery_mv;
        turtleBatteryPct = m.battery_pct > 100 ? 100 : m.battery_pct;
        lastTelemetryMs = millis();
        lastAckMs = millis();  // si la tortuga habla, el enlace esta vivo
    }
}

bool turtleLinkOk() {
    return lastAckMs != 0 && millis() - lastAckMs < LINK_TIMEOUT_MS;
}

// ==========================================
// 고래 배터리 표시 (코드로 생성, SquareLine 수정 불필요)
// 모든 화면 왼쪽 위, 거북이 배터리 바로 옆에 같은 스타일로 표시됨:
//
//   [거북이 배터리 🐢]  [ 85% ]><   ← 고래 몸통(바) + 꼬리
//
// 위치/크기는 아래 상수로 조정 (거북이 아이콘과 겹치면 BALLENA_BAT_X 를 늘리기)
constexpr lv_coord_t BALLENA_BAT_X = 104;   // 왼쪽 위 기준 x
constexpr lv_coord_t BALLENA_BAT_Y = 12;
constexpr lv_coord_t BALLENA_BAT_W = 64;    // 몸통(바) 폭
constexpr lv_coord_t BALLENA_BAT_H = 24;    // 몸통(바) 높이
constexpr uint32_t BALLENA_NEON = 0x4FD8F6; // 테두리/꼬리 색 (거북이 아이콘과 같은 하늘색 네온)

// 화면마다 하나씩 (거북이 배터리처럼). 각 화면 안에 있어서
// 연결 상태 창(CONEXION BLUETOOTH)과 EMG 그래프보다 항상 뒤에 그려짐.
constexpr int BALLENA_N = 6;
static lv_obj_t *ballenaBar[BALLENA_N] = {NULL};
static lv_obj_t *ballenaTxt[BALLENA_N] = {NULL};

static void createBallenaBatteryOn(lv_obj_t *scr, int i) {
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, BALLENA_BAT_W + 26, BALLENA_BAT_H);
    lv_obj_set_pos(box, BALLENA_BAT_X, BALLENA_BAT_Y);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    // 몸통 = 배터리 바
    lv_obj_t *bar = lv_bar_create(box);
    lv_obj_set_size(bar, BALLENA_BAT_W, BALLENA_BAT_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x06243F), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(BALLENA_NEON), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x3CCB5A), LV_PART_INDICATOR);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    // 꼬리 (선으로 그린 고래 꼬리)
    static lv_point_t tail[] = {{0, 12}, {8, 12}, {14, 4}, {22, 1}, {19, 9}, {13, 12},
                                {19, 15}, {22, 23}, {14, 20}, {8, 12}};
    lv_obj_t *t = lv_line_create(box);
    lv_line_set_points(t, tail, sizeof(tail) / sizeof(tail[0]));
    lv_obj_set_pos(t, BALLENA_BAT_W, 0);
    lv_obj_set_style_line_width(t, 2, 0);
    lv_obj_set_style_line_color(t, lv_color_hex(BALLENA_NEON), 0);
    lv_obj_set_style_line_rounded(t, true, 0);

    // 몸통 안의 % 글자
    lv_obj_t *txt = lv_label_create(bar);
    lv_label_set_text(txt, "--");
    lv_obj_set_style_text_color(txt, lv_color_white(), 0);
    lv_obj_set_style_text_font(txt, &lv_font_montserrat_14, 0);
    lv_obj_center(txt);

    ballenaBar[i] = bar;
    ballenaTxt[i] = txt;
}

// setup() 에서 createConnPanel() 보다 먼저 호출해야 연결 창 아래에 깔림
static void createBallenaBattery() {
    lv_obj_t *screens[BALLENA_N] = {ui_INICIO, ui_CONEXION_BLUETOOTH, ui_SELECCION_DE_MODO,
                                    ui_MODO_JUEGO, ui_MODO_REHABILITACION, ui_RESUMEN_DE_SESSION};
    for (int i = 0; i < BALLENA_N; i++) {
        if (screens[i] != NULL) createBallenaBatteryOn(screens[i], i);
    }
}

// Lee la pila en la pantalla cada 0.5 s (promedio de 8 lecturas + filtro para que no brinque)
static void ballenaBatLocalUpdate() {
    if (BALLENA_BAT_PIN < 0) return;
    static uint32_t last = 0;
    static float vFilt = 0;
    if (millis() - last < 500) return;
    last = millis();
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(BALLENA_BAT_PIN);
    const float v = acc / 8 / 1000.0f * BALLENA_BAT_DIVIDER;
    if (v < 5.0f || v > 11.0f) {   // sin pila (solo USB) o sin divisor → "--" despues del timeout
        vFilt = 0;
        return;
    }
    vFilt = (vFilt == 0) ? v : vFilt * 0.8f + v * 0.2f;
    float pct = (vFilt - BALLENA_BAT_V_EMPTY) / (BALLENA_BAT_V_FULL - BALLENA_BAT_V_EMPTY) * 100.0f;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    ballenaBatteryMv = (uint16_t)(vFilt * 1000.0f);
    ballenaBatteryPct = (uint8_t)(pct + 0.5f);
    lastBallenaBatMs = millis();
}

static void updateBallenaBattery() {
    ballenaBatLocalUpdate();
    static int lastShown = -2;
    bool fresh = lastBallenaBatMs != 0 && millis() - lastBallenaBatMs < BATTERY_TIMEOUT_MS;
    int pct = fresh ? ballenaBatteryPct : -1;
    if (pct == lastShown) return;
    lastShown = pct;

    char txt[8];
    if (pct < 0) snprintf(txt, sizeof(txt), "--");
    else snprintf(txt, sizeof(txt), "%d%%", pct);
    lv_color_t col = lv_color_hex(pct >= 0 && pct < BATTERY_LOW_PCT ? 0xE8231E : 0x3CCB5A);
    for (int i = 0; i < BALLENA_N; i++) {
        if (ballenaBar[i] == NULL) continue;
        lv_label_set_text(ballenaTxt[i], txt);
        lv_obj_center(ballenaTxt[i]);
        lv_bar_set_value(ballenaBar[i], pct < 0 ? 0 : pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(ballenaBar[i], col, LV_PART_INDICATOR);
    }
    if (pct >= 0) {
        Serial.printf("[BATERIA] ballena %u mV = %d%%\n", (unsigned)ballenaBatteryMv, pct);
    }
}
// ==========================================

// ==========================================
// 거북이 배터리 표시 (코드로 생성, SquareLine 수정 불필요)
// 배경 이미지에 그려진 거북이 모양과 SquareLine 의 BatteryBAR / lblBatteryTxt 대신,
// 고래 배터리와 같은 방식으로 코드에서 그림:
//
//   ◁[ 85% ]◯·      ← 꼬리 + 등껍질(바) + 머리, 아래에 발 2개
//
// - 배경 이미지의 거북이는 검은 사각형으로 덮음 (TORTUGA_COVER_*)
// - 각 화면 안에 만들어서 연결 상태 창과 EMG 그래프보다 항상 뒤에 그려짐
// - 위치: 고래 배터리와 같은 높이 (BALLENA_BAT_Y 를 바꾸면 같이 움직임)
constexpr lv_coord_t TORTUGA_BAT_X = 4;
constexpr lv_coord_t TORTUGA_BAT_Y = 8;
constexpr lv_coord_t TORTUGA_SHELL_W = 58;               // 등껍질(바) 폭
constexpr lv_coord_t TORTUGA_SHELL_H = BALLENA_BAT_H;    // 등껍질(바) 높이
// 배경 이미지의 거북이(x 5~88, y 5~39)를 덮는 검은 사각형. 아래 하늘색 선(y 46)은 안 덮음.
// 덮인 부분이 보이면: TORTUGA_COVER_COLOR 를 0xFF0000 으로 바꿔서 범위를 확인하고 조정
constexpr lv_coord_t TORTUGA_COVER_X = 0;
constexpr lv_coord_t TORTUGA_COVER_Y = 0;
constexpr lv_coord_t TORTUGA_COVER_W = 100;
constexpr lv_coord_t TORTUGA_COVER_H = 45;
constexpr uint32_t TORTUGA_COVER_COLOR = 0x000000;

constexpr int TORTUGA_N = 6;
static lv_obj_t *tortugaBar[TORTUGA_N] = {NULL};
static lv_obj_t *tortugaTxt[TORTUGA_N] = {NULL};

static lv_obj_t *tortugaLine(lv_obj_t *parent, const lv_point_t *pts, uint16_t n) {
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, pts, n);
    lv_obj_set_pos(l, 0, 0);
    lv_obj_set_style_line_width(l, 2, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(BALLENA_NEON), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    return l;
}

static void createTortugaBatteryOn(lv_obj_t *scr, int i) {
    // 1) 배경 이미지의 거북이 덮기
    lv_obj_t *cover = lv_obj_create(scr);
    lv_obj_remove_style_all(cover);
    lv_obj_set_pos(cover, TORTUGA_COVER_X, TORTUGA_COVER_Y);
    lv_obj_set_size(cover, TORTUGA_COVER_W, TORTUGA_COVER_H);
    lv_obj_set_style_bg_color(cover, lv_color_hex(TORTUGA_COVER_COLOR), 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);

    // 2) 코드로 그린 거북이 (꼬리 10 px + 등껍질 + 머리 20 px)
    const lv_coord_t sx = 10;                       // 등껍질 시작 x (상자 안)
    const lv_coord_t ex = sx + TORTUGA_SHELL_W;     // 등껍질 끝 x
    const lv_coord_t H = TORTUGA_SHELL_H;
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, ex + 22, H + 8);
    lv_obj_set_pos(box, TORTUGA_BAT_X, TORTUGA_BAT_Y);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    // 꼬리 (왼쪽 작은 삼각형)
    static lv_point_t tail[] = {{sx, 8}, {1, 9}, {3, 15}, {sx, 15}};
    tortugaLine(box, tail, 4);
    // 발 2개 (등껍질 아래)
    static lv_point_t footL[] = {{sx + 5, H - 1}, {sx + 4, H + 6}, {sx + 13, H + 6}, {sx + 13, H - 1}};
    static lv_point_t footR[] = {{ex - 14, H - 1}, {ex - 14, H + 6}, {ex - 5, H + 6}, {ex - 6, H - 1}};
    tortugaLine(box, footL, 4);
    tortugaLine(box, footR, 4);

    // 머리 (동그라미 + 눈)
    lv_obj_t *head = lv_obj_create(box);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, 18, 18);
    lv_obj_set_pos(head, ex - 1, (H - 18) / 2);   // 등껍질에 붙어 있게
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(head, lv_color_hex(BALLENA_NEON), 0);
    lv_obj_set_style_border_width(head, 2, 0);
    lv_obj_set_style_bg_color(head, lv_color_hex(0x06243F), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_t *eye = lv_obj_create(head);
    lv_obj_remove_style_all(eye);
    lv_obj_set_size(eye, 4, 4);
    lv_obj_align(eye, LV_ALIGN_CENTER, 2, -1);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye, lv_color_hex(BALLENA_NEON), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);

    // 등껍질 = 배터리 바 (고래 몸통과 같은 스타일)
    lv_obj_t *bar = lv_bar_create(box);
    lv_obj_set_size(bar, TORTUGA_SHELL_W, H);
    lv_obj_set_pos(bar, sx, 0);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x06243F), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(BALLENA_NEON), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x3CCB5A), LV_PART_INDICATOR);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *txt = lv_label_create(bar);
    lv_label_set_text(txt, "--");
    lv_obj_set_style_text_color(txt, lv_color_white(), 0);
    lv_obj_set_style_text_font(txt, &lv_font_montserrat_14, 0);
    lv_obj_center(txt);

    tortugaBar[i] = bar;
    tortugaTxt[i] = txt;
}

// setup() 에서 createBallenaBattery() / createConnPanel() 보다 먼저 호출
static void createTortugaBattery() {
    // SquareLine 의 거북이 배터리 바/글자는 숨김 (코드의 거북이로 대체)
    lv_obj_t *old[] = {ui_BatteryBAR1, ui_BatteryBAR2, ui_BatteryBAR3,
                       ui_BatteryBAR4, ui_BatteryBAR5, ui_BatteryBAR6,
                       ui_lblBatteryTxt1, ui_lblBatteryTxt2, ui_lblBatteryTxt3,
                       ui_lblBatteryTxt4, ui_lblBatteryTxt5, ui_lblBatteryTxt6};
    for (lv_obj_t *o : old) {
        if (o != NULL) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *screens[TORTUGA_N] = {ui_INICIO, ui_CONEXION_BLUETOOTH, ui_SELECCION_DE_MODO,
                                    ui_MODO_JUEGO, ui_MODO_REHABILITACION, ui_RESUMEN_DE_SESSION};
    for (int i = 0; i < TORTUGA_N; i++) {
        if (screens[i] != NULL) createTortugaBatteryOn(screens[i], i);
    }
}
// ==========================================

// Actualiza las 6 barras/etiquetas de bateria (una por pantalla)
static void updateBatteryUI() {
    updateBallenaBattery();   // 고래 배터리도 같이 갱신
    static uint32_t lastUpdate = 0;
    static int lastShown = -2;
    if (millis() - lastUpdate < 500) return;
    lastUpdate = millis();

    bool fresh = lastTelemetryMs != 0 && millis() - lastTelemetryMs < BATTERY_TIMEOUT_MS;
    int pct = fresh ? turtleBatteryPct : -1;
    if (pct == lastShown) return;
    lastShown = pct;

    char txt[8];
    if (pct < 0) snprintf(txt, sizeof(txt), "--");
    else snprintf(txt, sizeof(txt), "%d%%", pct);
    lv_color_t col = lv_color_hex(pct >= 0 && pct < BATTERY_LOW_PCT ? 0xE8231E : 0x3CCB5A);

    // 코드로 만든 거북이 배터리 (createTortugaBattery)
    for (int i = 0; i < TORTUGA_N; i++) {
        if (tortugaBar[i] == NULL) continue;
        lv_label_set_text(tortugaTxt[i], txt);
        lv_obj_center(tortugaTxt[i]);
        lv_bar_set_value(tortugaBar[i], pct < 0 ? 0 : pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(tortugaBar[i], col, LV_PART_INDICATOR);
    }
    if (pct >= 0) {
        Serial.printf("[BATERIA] tortuga %u mV = %d%%\n", (unsigned)turtleBatteryMv, pct);
    }
}

static uint8_t lastSentCmd = MOTOR_CMD_STOP;
static uint8_t lastSentSpeed = 0;
static uint32_t lastSendMs = 0;

void sendMotor(uint8_t cmd, uint8_t speed) {
    MotorMsg msg = {MOTOR_MSG_MAGIC, cmd, speed};
    esp_err_t result = esp_now_send(turtleAddress, (uint8_t *)&msg, sizeof(msg));
    if (result != ESP_OK) {
        // 1초에 한 번만 출력 (100 ms 마다 찍으면 Serial 이 막힘)
        static uint32_t lastErrMs = 0;
        static uint32_t errCount = 0;
        errCount++;
        if (millis() - lastErrMs > 1000) {
            Serial.printf("Error al enviar comando %u (x%lu, err %d)\n", cmd, (unsigned long)errCount, (int)result);
            lastErrMs = millis();
            errCount = 0;
        }
    }
    lastSentCmd = cmd;
    lastSentSpeed = speed;
    lastSendMs = millis();
}

bool isPressed(lv_obj_t *btn) {
    return btn != NULL && lv_obj_has_state(btn, LV_STATE_PRESSED);
}

// WT32-SC01 Plus 전용 LovyanGFX 하드웨어 설정 클래스
class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7796 _panel_instance;
    lgfx::Bus_Parallel8 _bus_instance;
    lgfx::Touch_FT5x06 _touch_instance;

public:
    LGFX(void)
    {
        // 8비트 병렬 버스 설정 (ESP32-S3 핀맵 적용)
        auto cfg = _bus_instance.config();
        cfg.freq_write = 20000000;
        cfg.pin_wr = 47;
        cfg.pin_rd = -1;
        cfg.pin_rs = 0; // DC pin
        cfg.pin_d0 = 9;
        cfg.pin_d1 = 46;
        cfg.pin_d2 = 3;
        cfg.pin_d3 = 8;
        cfg.pin_d4 = 18;
        cfg.pin_d5 = 17;
        cfg.pin_d6 = 16;
        cfg.pin_d7 = 15;
        _bus_instance.config(cfg);
        _panel_instance.setBus(&_bus_instance);

        // ST7796U (내부 패널 부품) 패널 설정
        auto panel_cfg = _panel_instance.config();
        panel_cfg.pin_cs = -1;
        panel_cfg.pin_rst = 4;
        panel_cfg.pin_busy = -1;
        panel_cfg.memory_width = 320;
        panel_cfg.memory_height = 480;
        panel_cfg.panel_width = 320;
        panel_cfg.panel_height = 480;
        panel_cfg.offset_x = 0;
        panel_cfg.offset_y = 0;

        panel_cfg.offset_rotation = 0; // 가로 모드
        panel_cfg.dummy_read_pixel = 8;
        panel_cfg.dummy_read_bits = 1;
        panel_cfg.readable = true;
        panel_cfg.invert = true; // 색상 반전 자동 보정

        panel_cfg.rgb_order = false;
        panel_cfg.dlen_16bit = false;
        panel_cfg.bus_shared = false;
        _panel_instance.config(panel_cfg);

        // 터치 패널 설정 (FT6336U)
        auto touch_cfg = _touch_instance.config();
        touch_cfg.x_min = 0;
        touch_cfg.x_max = 320;
        touch_cfg.y_min = 0;
        touch_cfg.y_max = 480;
        touch_cfg.pin_int = 7;
        touch_cfg.pin_rst = -1;
        touch_cfg.pin_sda = 6; // 터치 전용 SDA 핀
        touch_cfg.pin_scl = 5;
        touch_cfg.freq = 400000;
        touch_cfg.i2c_port = 0;
        touch_cfg.i2c_addr = 0x38; // FT6336U I2C 주소
        _touch_instance.config(touch_cfg);
        _panel_instance.setTouch(&_touch_instance);

        setPanel(&_panel_instance);
    }
};

LGFX tft;

static const uint16_t screenWidth = 480;
static const uint16_t screenHeight = 320;

static lv_disp_draw_buf_t draw_buf;
// Pantalla mas fluida: 2 buffers + DMA. Mientras un pedazo se manda a la pantalla,
// LVGL ya dibuja el siguiente. false = como antes (1 buffer, sin DMA).
constexpr bool DISPLAY_DMA = true;
constexpr uint32_t DRAW_BUF_LINES = 20;   // 2 x 20 lineas = la misma RAM que antes (1 x 40): HTTPS necesita RAM interna
static lv_color_t *drawBuf1 = NULL;
static lv_color_t *drawBuf2 = NULL;

// ============ ESPEJO DE PANTALLA POR USB ============
// La misma imagen de la pantalla, en grande, en la computadora conectada por el cable USB.
// En la computadora: abrir tools/espejo.html en Chrome o Edge → Conectar → elegir el puerto.
// Solo manda imagen mientras esa pagina esta abierta (la pagina manda 'M' cada segundo);
// sin la pagina no cambia nada. Antes de subir el programa, cerrar la pagina (usa el puerto).
// Paquete: "LVM1" + x,y,w,h (uint16) + w*h pixeles RGB565 + suma (uint16), todo little-endian.
constexpr bool ESPEJO_USB = true;              // false = apagado del todo
constexpr uint32_t ESPEJO_TIMEOUT_MS = 3000;   // sin 'M' en este tiempo → se apaga solo
constexpr uint32_t ESPEJO_TX_BUFFER = 4096;    // buffer de salida del USB (RAM interna)
constexpr uint32_t ESPEJO_PKT_PIX = 960;       // pixeles por paquete (2 filas de 480)
constexpr uint32_t ESPEJO_US_POR_LOOP = 4000;  // tiempo maximo mandando por vuelta del loop

static uint16_t *espejoFb = NULL;   // copia de la pantalla en PSRAM (300 KB)
static bool espejoOn = false;
static uint32_t espejoUltimoM = 0;
static int16_t espSucX1 = 0, espSucY1 = 0, espSucX2 = -1, espSucY2 = -1;   // zona cambiada, falta mandar
static int16_t espEnvX1 = 0, espEnvX2 = -1, espEnvY = 0, espEnvY2 = -1;    // zona que se esta mandando
static uint8_t espejoPkt[12 + ESPEJO_PKT_PIX * 2 + 2];

static void espejoMarcar(int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    if (espSucX2 < espSucX1) {
        espSucX1 = x1; espSucY1 = y1; espSucX2 = x2; espSucY2 = y2;
    } else {
        if (x1 < espSucX1) espSucX1 = x1;
        if (y1 < espSucY1) espSucY1 = y1;
        if (x2 > espSucX2) espSucX2 = x2;
        if (y2 > espSucY2) espSucY2 = y2;
    }
}

// se llama desde my_disp_flush: copia el pedazo dibujado (rapido, sin USB)
static inline void espejoCopiar(const lv_area_t *a, const lv_color_t *src) {
    if (!espejoOn || espejoFb == NULL) return;
    if (a->x1 < 0 || a->y1 < 0 || a->x2 >= screenWidth || a->y2 >= screenHeight) return;
    const int16_t w = a->x2 - a->x1 + 1;
    const uint16_t *s = (const uint16_t *)src;
    for (int16_t y = a->y1; y <= a->y2; y++, s += w) {
        memcpy(&espejoFb[(uint32_t)y * screenWidth + a->x1], s, w * 2);
    }
    espejoMarcar(a->x1, a->y1, a->x2, a->y2);
}

static void espejoApagar() {
    espejoOn = false;
    espSucX2 = -1; espSucX1 = 0;
    espEnvY2 = -1; espEnvY = 0;
}

// comandos de la pagina (llegan por emgSerialUpdate): M = encender / sigo aqui, m = apagar, F = todo otra vez
// ---- Toque desde la computadora (clic en tools/espejo.html) ----
// La pagina manda "X" + x (3 cifras) + y (3 cifras) + "1" (presionado) o "0" (suelto), p. ej. "X2401601".
// Mientras se mantiene presionado lo repite cada 150 ms; sin repeticion en 400 ms se suelta solo
// (por si se cierra la pagina con el boton presionado). Solo funciona con el espejo encendido.
constexpr uint32_t ESPEJO_TOQUE_TIMEOUT_MS = 400;
static bool remTouchPressed = false;
static int16_t remTouchX = 0, remTouchY = 0;
static uint32_t remTouchMs = 0;
static uint32_t remMinHasta = 0;   // un clic rapido se mantiene al menos 80 ms (LVGL lee el toque cada 30 ms)
static char remBuf[7];
static uint8_t remLen = 0;
static bool remLeyendo = false;
static uint32_t remInicioMs = 0;

static void remTouchParse() {
    for (int i = 0; i < 6; i++) if (remBuf[i] < '0' || remBuf[i] > '9') return;
    if (remBuf[6] != '0' && remBuf[6] != '1') return;
    const int x = (remBuf[0] - '0') * 100 + (remBuf[1] - '0') * 10 + (remBuf[2] - '0');
    const int y = (remBuf[3] - '0') * 100 + (remBuf[4] - '0') * 10 + (remBuf[5] - '0');
    if (x >= screenWidth || y >= screenHeight || !espejoOn) return;
    remTouchX = x;
    remTouchY = y;
    const bool pr = remBuf[6] == '1';
    if (pr && !remTouchPressed) remMinHasta = millis() + 80;
    remTouchPressed = pr;
    remTouchMs = millis();
}

// lo usa my_touchpad_read: true = la computadora esta "tocando" la pantalla
static bool remTouchActive(int16_t *x, int16_t *y) {
    if (!espejoOn) return false;
    const bool sostenido = remTouchPressed && millis() - remTouchMs <= ESPEJO_TOQUE_TIMEOUT_MS;
    const bool clicCorto = (int32_t)(remMinHasta - millis()) > 0;
    if (!sostenido && !clicCorto) return false;
    *x = remTouchX;
    *y = remTouchY;
    return true;
}

bool mirrorSerialCommand(char c) {
    if (!ESPEJO_USB) return false;
    if (remLeyendo) {
        if (millis() - remInicioMs > 200) {
            remLeyendo = false;   // mensaje incompleto: se descarta y este caracter se procesa normal
        } else {
            remBuf[remLen++] = c;
            if (remLen == sizeof(remBuf)) {
                remLeyendo = false;
                remTouchParse();
            }
            return true;
        }
    }
    if (c == 'X') {
        remLeyendo = true;
        remLen = 0;
        remInicioMs = millis();
        return true;
    }
    if (c == 'M') {
        if (!espejoOn && espejoFb != NULL) {
            espejoOn = true;
            lv_obj_invalidate(lv_scr_act());   // redibuja todo → la copia queda completa
            Serial.println("[ESPEJO] encendido");
        }
        espejoUltimoM = millis();
        return true;
    }
    if (c == 'F') {
        if (espejoOn) espejoMarcar(0, 0, screenWidth - 1, screenHeight - 1);
        return true;
    }
    if (c == 'm') {
        if (espejoOn) Serial.println("[ESPEJO] apagado");
        espejoApagar();
        return true;
    }
    return false;
}

// en el loop: manda lo que cambio, por pedazos, solo si cabe en el buffer (nunca detiene la pantalla)
void espejoUpdate() {
    if (!ESPEJO_USB || !espejoOn) return;
    if (millis() - espejoUltimoM > ESPEJO_TIMEOUT_MS) {   // se cerro la pagina
        espejoApagar();
        return;
    }
    const uint32_t t0 = micros();
    while (micros() - t0 < ESPEJO_US_POR_LOOP) {
        if (espEnvY > espEnvY2) {                 // termino la zona anterior: tomar la nueva
            if (espSucX2 < espSucX1) return;      // nada que mandar
            espEnvX1 = espSucX1; espEnvX2 = espSucX2;
            espEnvY = espSucY1;  espEnvY2 = espSucY2;
            espSucX2 = -1; espSucX1 = 0;
        }
        const uint16_t w = espEnvX2 - espEnvX1 + 1;
        uint16_t rows = ESPEJO_PKT_PIX / w;
        if (rows < 1) rows = 1;
        if (rows > espEnvY2 - espEnvY + 1) rows = espEnvY2 - espEnvY + 1;
        const size_t nPix = (size_t)w * rows;
        const size_t n = 12 + nPix * 2 + 2;
        if (Serial.availableForWrite() < (int)(n + 256)) return;   // deja lugar para los textos del Serial

        uint8_t *p = espejoPkt;
        p[0] = 'L'; p[1] = 'V'; p[2] = 'M'; p[3] = '1';
        const uint16_t hdr[4] = {(uint16_t)espEnvX1, (uint16_t)espEnvY, w, rows};
        memcpy(p + 4, hdr, 8);
        uint16_t *px = (uint16_t *)(p + 12);
        uint16_t suma = 0;
        for (uint16_t r = 0; r < rows; r++) {
            const uint16_t *src = &espejoFb[(uint32_t)(espEnvY + r) * screenWidth + espEnvX1];
            for (uint16_t i = 0; i < w; i++) {
                const uint16_t v = src[i];
                *px++ = v;
                suma += v;
            }
        }
        memcpy(p + 12 + nPix * 2, &suma, 2);
        Serial.write(p, n);
        espEnvY += rows;
    }
}
// ==========================================

// LVGL 디스플레이 플러시 함수
void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    espejoCopiar(area, color_p);

    if (DISPLAY_DMA && drawBuf2 != NULL) {
        // la transaccion queda abierta; pushImageDMA espera a que termine el pedazo anterior
        if (tft.getStartCount() == 0) tft.startWrite();
        tft.pushImageDMA(area->x1, area->y1, w, h, (lgfx::rgb565_t *)&color_p->full);
    } else {
        tft.startWrite();
        tft.pushImage(area->x1, area->y1, w, h, (lgfx::rgb565_t *)&color_p->full);
        tft.endWrite();
    }
    lv_disp_flush_ready(disp_drv);
}

// 터치패드 읽기 함수 (LovyanGFX 터치 연동)
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    uint16_t touchX, touchY;
    bool touched = tft.getTouch(&touchX, &touchY);
    int16_t rx, ry;

    if (!touched && remTouchActive(&rx, &ry)) {   // clic desde la computadora (espejo)
        data->state = LV_INDEV_STATE_PR;
        data->point.x = rx;
        data->point.y = ry;
    } else if (!touched) {
        data->state = LV_INDEV_STATE_REL;
    } else {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touchX;
        data->point.y = touchY;
    }
}

unsigned long lastMillis = 0;
int btLoadingValue = 0;
bool isBluetoothConnected = false;


// ==========================================
// 게임 모드 (MODO JUEGO)

int puntos = 0;           // 누적 점수 카운터
int lastColor = -1;       // 이전에 나온 색상을 기억하여 중복을 방지하는 변수

// Mascara de la nube (ui_img_nube.c, archivo C)
extern "C" {
LV_IMG_DECLARE(ui_img_nube);
extern const lv_coord_t ui_img_nube_x;
extern const lv_coord_t ui_img_nube_y;
}
lv_obj_t *ui_Nube4 = NULL;

// 1. 점수를 화면(lblPOINT4)에 업데이트하는 함수
void updatePuntosUI() {
    if (ui_lblPOINT4 != NULL) {
        char ptsStr[10];
        sprintf(ptsStr, "%d", puntos);
        lv_label_set_text(ui_lblPOINT4, ptsStr);
    }
}

// 현재 화면에 표시된 색상의 이름을 스피커로 말하기
// 음성은 효과음(정답/오답)을 기다리지 않고 바로 재생됨 (audio.cpp 의 음성 채널)
void speakCurrentColor() {
    static const Sfx voices[] = {SFX_ROJO, SFX_AMARILLO, SFX_VERDE, SFX_AZUL};
    if (lastColor >= 0 && lastColor < 4) {
        audioPlaySfx(voices[lastColor]);
    }
}

// 2. 랜덤 색상을 뽑고 텍스트와 글자 색상을 바꾸는 함수
// speak = true: ademas se oye la voz con el nombre del color
void generateRandomColor(bool speak = true) {
    if (ui_lblCOLOR4 == NULL) return;

    int newColor;
    // 이전 색상과 동일하지 않은 색상이 나올 때까지 무한 반복 (연속 중복 방지)
    do {
        newColor = random(0, 4); // 0~3 사이의 난수 생성
    } while (newColor == lastColor);

    lastColor = newColor; // 새로 뽑은 색상을 저장

    // 텍스트 색상
    static const char *names[] = {"ROJO", "AMARILLO", "VERDE", "AZUL"};
    static const uint32_t colors[] = {0xE8231E, 0xFFE81A, 0x1FB83A, 0x6EC6FF};
    lv_label_set_text(ui_lblCOLOR4, names[newColor]);
    // SquareLine 에서 지정한 흰색(0xFFFFFF)을 덮어써서, 글자 색을 해당 색상으로 바꿈
    lv_obj_set_style_text_color(ui_lblCOLOR4, lv_color_hex(colors[newColor]),
                                LV_PART_MAIN | LV_STATE_DEFAULT);

    if (speak) {
        speakCurrentColor();
    }
}

// Relleno de color de la nube de MODO JUEGO: mascara con la forma del interior
// de la nube del fondo (ui_img_nube.c, generada con tools/make_nube_mask.py).
// LVGL pinta las imagenes ALPHA_8BIT con el color de img_recolor.
void createNube() {
    if (ui_MODO_JUEGO == NULL) return;

    ui_Nube4 = lv_img_create(ui_MODO_JUEGO);
    lv_img_set_src(ui_Nube4, &ui_img_nube);
    lv_obj_set_pos(ui_Nube4, ui_img_nube_x, ui_img_nube_y);
    lv_obj_clear_flag(ui_Nube4, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_img_opa(ui_Nube4, 230, LV_PART_MAIN);
    lv_obj_move_to_index(ui_Nube4, 1);   // justo encima del fondo, debajo del texto
}

// Sonidos de los botones. Van en LV_EVENT_PRESSED (al tocar), no en CLICKED (al
// soltar el dedo, que es donde SquareLine pone las acciones): asi se oyen sin retraso.
static void bubbleEvent(lv_event_t * e) {
    audioPlaySfx(SFX_BUBBLE);
}

static void palomitaSoundEvent(lv_event_t * e) {
    audioPlaySfx(SFX_CORRECT);  // la voz del nuevo color suena encima (no espera)
}

static void tacheSoundEvent(lv_event_t * e) {
    audioPlaySfx(SFX_WRONG);
}

void addBubbleToButtons() {
    lv_obj_t *screens[] = {ui_INICIO, ui_CONEXION_BLUETOOTH, ui_SELECCION_DE_MODO,
                           ui_MODO_JUEGO, ui_MODO_REHABILITACION, ui_RESUMEN_DE_SESSION};
    for (lv_obj_t *scr : screens) {
        if (scr == NULL) continue;
        for (uint32_t i = 0; i < lv_obj_get_child_cnt(scr); i++) {
            lv_obj_t *child = lv_obj_get_child(scr, i);
            if (lv_obj_check_type(child, &lv_btn_class) && child != ui_PALOMITA && child != ui_TACHE) {
                lv_obj_add_event_cb(child, bubbleEvent, LV_EVENT_PRESSED, NULL);
            }
        }
    }
    if (ui_PALOMITA != NULL) lv_obj_add_event_cb(ui_PALOMITA, palomitaSoundEvent, LV_EVENT_PRESSED, NULL);
    if (ui_TACHE != NULL) lv_obj_add_event_cb(ui_TACHE, tacheSoundEvent, LV_EVENT_PRESSED, NULL);
}

// ==========================================
// 볼륨 조절 (코드로 생성, SquareLine 수정 불필요)
//
// 평소: 고래 배터리 오른쪽에 작은 스피커 버튼만 보임.
//       버튼은 각 화면 안에 만들어져서, 연결 상태 창과 EMG 그래프보다
//       항상 뒤에 그려짐 (고래 배터리와 같은 방식) = 가리지 않음.
// 누르면: 버튼 아래에 작은 볼륨 창이 열림
//       [스피커] 전체 볼륨 (음성 + 효과음 + 음악)
//       [음표]   배경 음악만
//       창 밖을 누르거나 5초 동안 만지지 않으면 자동으로 닫힘.
// 볼륨은 전원을 꺼도 유지됨 (audio.cpp 가 NVS 에 저장).
constexpr lv_coord_t VOL_BTN_X = BALLENA_BAT_X + BALLENA_BAT_W + 34;  // 고래 꼬리 바로 오른쪽
constexpr lv_coord_t VOL_BTN_Y = BALLENA_BAT_Y;
constexpr lv_coord_t VOL_BTN_W = 40;
constexpr lv_coord_t VOL_BTN_H = BALLENA_BAT_H;
constexpr lv_coord_t VOL_PANEL_W = 300;
constexpr lv_coord_t VOL_PANEL_H = 100;
constexpr uint32_t VOL_AUTOCLOSE_MS = 5000;

constexpr int VOL_N = 6;
static lv_obj_t *volBtnLabel[VOL_N] = {NULL};
static lv_obj_t *volBackdrop = NULL;   // 화면 전체를 덮는 투명 판 (창 밖 터치 = 닫기)
static lv_obj_t *volPanel = NULL;
static lv_obj_t *volSlider = NULL;
static lv_obj_t *volValue = NULL;
static lv_obj_t *volIcon = NULL;
static lv_obj_t *musSlider = NULL;
static lv_obj_t *musValue = NULL;
static uint32_t volLastTouchMs = 0;

// Limite real del volumen general: el amplificador se calienta mucho arriba de 70 %.
// En pantalla se ve 0-100 %, pero 100 % en pantalla = VOL_REAL_MAX % real.
constexpr int VOL_REAL_MAX = 70;
static int volUiFromReal(int r) {
    int u = (r * 100 + VOL_REAL_MAX / 2) / VOL_REAL_MAX;
    return u > 100 ? 100 : u;
}
static int volRealFromUi(int u) { return (u * VOL_REAL_MAX + 50) / 100; }

static const char *volSymbol(uint8_t pct) {
    if (pct == 0) return LV_SYMBOL_MUTE;
    if (pct < 50) return LV_SYMBOL_VOLUME_MID;
    return LV_SYMBOL_VOLUME_MAX;
}

static void volRefreshIcons() {
    const char *s = volSymbol(volUiFromReal(audioGetVolume()));
    for (int i = 0; i < VOL_N; i++) {
        if (volBtnLabel[i] != NULL) lv_label_set_text(volBtnLabel[i], s);
    }
    if (volIcon != NULL) lv_label_set_text(volIcon, s);
}

static void volClose() {
    if (volBackdrop != NULL) lv_obj_add_flag(volBackdrop, LV_OBJ_FLAG_HIDDEN);
}

static void volOpenEvent(lv_event_t *e) {
    if (volBackdrop == NULL) return;
    lv_slider_set_value(volSlider, volUiFromReal(audioGetVolume()), LV_ANIM_OFF);
    lv_slider_set_value(musSlider, audioGetMusicVolume(), LV_ANIM_OFF);
    lv_label_set_text_fmt(volValue, "%d%%", volUiFromReal(audioGetVolume()));
    lv_label_set_text_fmt(musValue, "%d%%", audioGetMusicVolume());
    volRefreshIcons();
    lv_obj_clear_flag(volBackdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(volBackdrop);   // EMG 그래프보다 앞 (사용자가 직접 연 창)
    volLastTouchMs = millis();
}

static void volBackdropEvent(lv_event_t *e) {
    if (lv_event_get_target(e) == volBackdrop) volClose();   // 창 바깥을 눌렀을 때만
}

static void volSliderChanged(lv_event_t *e) {
    lv_obj_t *s = lv_event_get_target(e);
    int v = lv_slider_get_value(s);
    volLastTouchMs = millis();
    if (s == volSlider) {
        audioSetVolume(volRealFromUi(v));   // pantalla 0-100 % → real 0-70 %
        lv_label_set_text_fmt(volValue, "%d%%", v);
        volRefreshIcons();
    } else {
        audioSetMusicVolume(v);
        lv_label_set_text_fmt(musValue, "%d%%", v);
    }
}

static void volSliderReleased(lv_event_t *e) {
    volLastTouchMs = millis();
    audioSaveVolume();                                       // 손을 뗄 때만 저장 (플래시 보호)
    if (lv_event_get_target(e) == volSlider) audioPlaySfx(SFX_BUBBLE);  // 바뀐 크기 미리 듣기
}

// 5초 동안 안 만지면 자동으로 닫기
static void volTimerCb(lv_timer_t *t) {
    if (volBackdrop == NULL || lv_obj_has_flag(volBackdrop, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_has_state(volSlider, LV_STATE_PRESSED) || lv_obj_has_state(musSlider, LV_STATE_PRESSED)) {
        volLastTouchMs = millis();
        return;
    }
    if (millis() - volLastTouchMs > VOL_AUTOCLOSE_MS) volClose();
}

static lv_obj_t *volRow(lv_coord_t y, const char *symbol, lv_obj_t **slider, lv_obj_t **value) {
    lv_obj_t *icon = lv_label_create(volPanel);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(icon, 0, y);

    lv_obj_t *s = lv_slider_create(volPanel);
    lv_obj_set_size(s, 176, 10);
    lv_obj_set_pos(s, 36, y + 5);
    lv_slider_set_range(s, 0, 100);
    lv_obj_set_ext_click_area(s, 14);   // 손가락으로 잡기 쉽게
    lv_obj_set_style_bg_color(s, lv_color_hex(0x1E5B8C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(BALLENA_NEON), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(s, volSliderChanged, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, volSliderReleased, LV_EVENT_RELEASED, NULL);

    lv_obj_t *v = lv_label_create(volPanel);
    lv_obj_set_width(v, 50);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(v, lv_color_white(), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, y + 1);

    *slider = s;
    *value = v;
    return icon;
}

static void createVolPanel() {
    volBackdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(volBackdrop);
    lv_obj_set_size(volBackdrop, 480, 320);
    lv_obj_set_pos(volBackdrop, 0, 0);
    lv_obj_add_flag(volBackdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(volBackdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(volBackdrop, volBackdropEvent, LV_EVENT_CLICKED, NULL);

    volPanel = lv_obj_create(volBackdrop);
    lv_obj_set_size(volPanel, VOL_PANEL_W, VOL_PANEL_H);
    lv_coord_t px = LV_CLAMP(4, VOL_BTN_X + VOL_BTN_W / 2 - VOL_PANEL_W / 2, 480 - VOL_PANEL_W - 4);
    lv_obj_set_pos(volPanel, px, VOL_BTN_Y + VOL_BTN_H + 6);
    lv_obj_set_style_bg_color(volPanel, lv_color_hex(0x06243F), 0);
    lv_obj_set_style_bg_opa(volPanel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(volPanel, lv_color_hex(BALLENA_NEON), 0);
    lv_obj_set_style_border_width(volPanel, 2, 0);
    lv_obj_set_style_radius(volPanel, 14, 0);
    lv_obj_set_style_pad_all(volPanel, 12, 0);
    lv_obj_clear_flag(volPanel, LV_OBJ_FLAG_SCROLLABLE);

    volIcon = volRow(4, LV_SYMBOL_VOLUME_MAX, &volSlider, &volValue);   // 전체
    volRow(44, LV_SYMBOL_AUDIO, &musSlider, &musValue);                 // 음악

    lv_obj_add_flag(volBackdrop, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(volTimerCb, 250, NULL);
}

static void createVolBtnOn(lv_obj_t *scr, int i) {
    lv_obj_t *b = lv_btn_create(scr);
    lv_obj_set_size(b, VOL_BTN_W, VOL_BTN_H);
    lv_obj_set_pos(b, VOL_BTN_X, VOL_BTN_Y);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x06243F), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(BALLENA_NEON), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_add_event_cb(b, bubbleEvent, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(b, volOpenEvent, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_label_set_text(l, LV_SYMBOL_VOLUME_MAX);
    lv_obj_center(l);
    volBtnLabel[i] = l;
}

// setup() 에서 createConnPanel() 보다 먼저 호출 (연결 창 아래에 깔리도록)
static void createVolumeControl() {
    lv_obj_t *screens[VOL_N] = {ui_INICIO, ui_CONEXION_BLUETOOTH, ui_SELECCION_DE_MODO,
                                ui_MODO_JUEGO, ui_MODO_REHABILITACION, ui_RESUMEN_DE_SESSION};
    for (int i = 0; i < VOL_N; i++) {
        if (screens[i] != NULL) createVolBtnOn(screens[i], i);
    }
    createVolPanel();
    volRefreshIcons();
}
// ==========================================

// 3. 물리치료사 정답 버튼
void Palomita(lv_event_t * e) {
    puntos++;               // 점수 1점 추가
    updatePuntosUI();       // 점수 화면 갱신
    generateRandomColor();  // 새로운 색상 제시
}

// 4. 물리치료사 오답 버튼
void Tache(lv_event_t * e) {
    generateRandomColor();  // 점수 추가 없이 새로운 색상만 제시
}

// 5. REGRESAR 버튼: 거북이를 REGRESAR_MS 동안 후진 (논블로킹)
// 실제 전송은 loop() 의 turtleUpdate() 가 100 ms 마다 함 (화면/EMG 가 멈추지 않음)
static uint32_t reverseUntilMs = 0;
static bool reversing = false;

void Regresar(lv_event_t * e) {
    Serial.println("Comando: REGRESAR (Turtle moves back)");
    reversing = true;
    reverseUntilMs = millis() + REGRESAR_MS;
}


// ==========================================
// 재활 모드 (MODO REHABILITACION)

int objetivoVal = 1;    // 시연을 위해 보기 좋게 초기 목표값을 1으로 설정
int progresoVal = 0;    // 환자가 현재까지 달성한 횟수
bool sessionCompleted = false; // 세션 완료 플래그
int ultimoModo = 1;     // 1 = MODO JUEGO, 2 = MODO REHABILITACION
static bool pendingSummary = false; // 목표 달성 → 마지막 수축이 끝나면 요약 화면으로

// 1. UI 텍스트, 바 동기화 함수 (재활 화면 & 요약 화면 동시 업데이트)
void updateRehabUI() {
    if (objetivoVal < 1) objetivoVal = 1;

    // 숫자를 문자열로 변환
    char objStr[10];
    sprintf(objStr, "%d", objetivoVal);

    char curStr[10];
    sprintf(curStr, "%d", progresoVal);

    // [재활 화면] 텍스트 업데이트 (위젯이 존재할 때만 실행하여 에러 방지)
    if (ui_lblTargetSET != NULL) lv_label_set_text(ui_lblTargetSET, objStr);
    if (ui_lblTargetCOUNT != NULL) lv_label_set_text(ui_lblTargetCOUNT, objStr);
    if (ui_lblCurrentCOUNT != NULL) lv_label_set_text(ui_lblCurrentCOUNT, curStr);

    // [재활 화면] 게이지 바 & 퍼센트 업데이트
    int pct = (objetivoVal > 0) ? ((progresoVal * 100) / objetivoVal) : 0;
    if (pct > 100) pct = 100;
    char pctStr[10];
    sprintf(pctStr, "%d%%", pct);
    if (ui_lblProgressPERCENT != NULL) lv_label_set_text(ui_lblProgressPERCENT, pctStr);
    if (ui_ProgressBAR != NULL) lv_bar_set_value(ui_ProgressBAR, pct, LV_ANIM_ON);

    // [요약 화면] 텍스트 미리 동기화 (화면 넘어가면 바로 숫자가 보이도록)
    if (ui_lblSummaryTarget != NULL) lv_label_set_text(ui_lblSummaryTarget, objStr);
    if (ui_lblSummaryAchieved != NULL) lv_label_set_text(ui_lblSummaryAchieved, curStr);
}

// 2. 물리치료사 화살표 버튼 이벤트 (목표 증가)
void TargetUP(lv_event_t * e) {
    if (objetivoVal < 99) {
        objetivoVal++;
        sessionCompleted = false;
        updateRehabUI(); // 즉시 화면에 숫자 반영
    }
}

// 3. 물리치료사 화살표 버튼 이벤트 (목표 감소)
void TargetDOWN(lv_event_t * e) {
    if (objetivoVal > 1) {
        objetivoVal--;
        // 목표 숫자를 내렸을 때 현재 달성 횟수보다 낮아지지 않도록 방지
        if (objetivoVal < progresoVal) progresoVal = objetivoVal;
        updateRehabUI(); // 즉시 화면에 숫자 반영
    }
}

static lv_obj_t *findChildLabel(lv_obj_t *btn);   // (아래 GRABAR 부분에 정의)
static void showToast(const char *msg, uint32_t ms, uint32_t color);

// 4. PAUSAR 버튼: 누를 때마다 일시정지 ↔ 재개 (토글)
//   일시정지: EMG 가 와도 거북이에게 명령을 보내지 않음 (STOP 만 보냄) → 거북이가 안 움직임
//   재개: 다시 EMG 에 따라 거북이가 움직임
static lv_obj_t *pausarLabel = NULL;
static char pausarOrigText[24] = "PAUSAR";

static void setPaused(bool p) {
    isGamePaused = p;
    if (p) sendMotor(MOTOR_CMD_STOP, 0);   // 즉시 정지
    sessionLogPause(p);                    // 녹화 중이면 일시정지 구간 기록
    if (pausarLabel != NULL) lv_label_set_text(pausarLabel, p ? "REANUDAR" : pausarOrigText);
    Serial.println(p ? "Sesion Pausada" : "Sesion Reanudada");
}

void PauseSession(lv_event_t * e) {
    if (pausarLabel == NULL && e != NULL) {
        pausarLabel = findChildLabel(lv_event_get_target(e));
        if (pausarLabel != NULL) {
            strncpy(pausarOrigText, lv_label_get_text(pausarLabel), sizeof(pausarOrigText) - 1);
        }
    }
    setPaused(!isGamePaused);
    showToast(isGamePaused ? LV_SYMBOL_PAUSE " Pausa: la tortuga no se mueve" : LV_SYMBOL_PLAY " Reanudado",
              1500, isGamePaused ? 0x8C5A1E : 0x1F8A3A);
}

// 5. CONTINUAR 버튼: 진행 횟수를 수동으로 +1 (물리치료사 보조용, 기존 동작)
//   100% 가 되면 바로 RESUMEN DE SESSION 으로 이동.
//   ※ 데이터에는 "MANUAL" 로 따로 기록됨 (EMG 로 센 반복과 구분)
void ContinueSession(lv_event_t * e) {
    if (sessionCompleted || progresoVal >= objetivoVal) return;
    progresoVal++;
    updateRehabUI();
    audioPlaySfx(SFX_CORRECT);
    sessionLogRep(millis(), 0, 0, -1, true);
    Serial.printf("REP MANUAL (CONTINUAR): %d/%d\n", progresoVal, objetivoVal);
    if (progresoVal >= objetivoVal) {
        sessionCompleted = true;
        ultimoModo = 2;
        pendingSummary = false;
        _ui_screen_change(&ui_RESUMEN_DE_SESSION, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, &ui_RESUMEN_DE_SESSION_screen_init);
    }
}

// 6. EMG 활성화 1회 = 반복 1회 (loop() 에서 활성화가 시작될 때 호출)
// 실제로 횟수가 올라갔으면 true
bool countRehabRep() {
    if (sessionCompleted || progresoVal >= objetivoVal) return false;

    progresoVal++;
    updateRehabUI();
    audioPlaySfx(SFX_CORRECT);  // 청각 보상

    if (progresoVal >= objetivoVal) {
        sessionCompleted = true;
        ultimoModo = 2;
        pendingSummary = true;   // 이번 수축이 끝나면 요약 화면으로 (거북이는 끝까지 전진)
    }
    return true;
}
// ==========================================


// ==========================================
// GRABAR / ENVIAR (MODO REHABILITACION)
// GRABAR: 이전 데이터를 지우고 지금부터 녹화 시작 (다시 누르면 처음부터 다시)
// ENVIAR: 녹화를 멈추고 지금까지의 데이터를 Wi-Fi 로 앱에 공개 (session_log.h 참고)

static lv_obj_t *grabarBtn = NULL;     // SquareLine 의 GRABAR 버튼 (첫 클릭 때 기억)
static lv_obj_t *grabarLabel = NULL;
static char grabarOrigText[24] = "GRABAR";
static lv_color_t grabarOrigColor;
static lv_obj_t *toastLabel = NULL;
static lv_timer_t *toastTimer = NULL;

static void toastHide(lv_timer_t *t) {
    if (toastLabel != NULL) lv_obj_add_flag(toastLabel, LV_OBJ_FLAG_HIDDEN);
    toastTimer = NULL;
}

// 화면 아래쪽에 잠깐 뜨는 안내 메시지
static void showToast(const char *msg, uint32_t ms, uint32_t color) {
    if (toastLabel == NULL) {
        toastLabel = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_color(toastLabel, lv_color_white(), 0);
        lv_obj_set_style_text_font(toastLabel, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(toastLabel, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_opa(toastLabel, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(toastLabel, 10, 0);
        lv_obj_set_style_radius(toastLabel, 12, 0);
        lv_obj_clear_flag(toastLabel, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_style_bg_color(toastLabel, lv_color_hex(color), 0);
    lv_label_set_text(toastLabel, msg);
    lv_obj_align(toastLabel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(toastLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toastLabel);
    if (toastTimer != NULL) lv_timer_del(toastTimer);
    toastTimer = lv_timer_create(toastHide, ms, NULL);
    lv_timer_set_repeat_count(toastTimer, 1);
}

static lv_obj_t *findChildLabel(lv_obj_t *btn) {
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(btn); i++) {
        lv_obj_t *c = lv_obj_get_child(btn, i);
        if (lv_obj_check_type(c, &lv_label_class)) return c;
    }
    return NULL;
}

// 녹화 중: GRABAR 버튼이 빨간 "REC 0:12" 로 바뀜
static void updateGrabarButton() {
    if (grabarBtn == NULL) return;
    static bool wasRec = false;
    static uint32_t lastSec = UINT32_MAX;
    bool rec = sessionLogRecording();
    if (rec) {
        uint32_t sec = sessionLogElapsedMs() / 1000;
        if (sec != lastSec || !wasRec) {
            lastSec = sec;
            char b[24];
            snprintf(b, sizeof(b), LV_SYMBOL_STOP " REC %lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
            if (grabarLabel != NULL) lv_label_set_text(grabarLabel, b);
            lv_obj_set_style_bg_color(grabarBtn, lv_color_hex(0xC81E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    } else if (wasRec) {
        if (grabarLabel != NULL) lv_label_set_text(grabarLabel, grabarOrigText);
        lv_obj_set_style_bg_color(grabarBtn, grabarOrigColor, LV_PART_MAIN | LV_STATE_DEFAULT);
        lastSec = UINT32_MAX;
    }
    wasRec = rec;
}

// ==========================================
// PACIENTE 버튼 (ContarPaciente): 환자 선택 창을 엶
//
//   ┌──────────────────────────────┐
//   │          Paciente            │
//   │   [ - ]   PAC-101   [ + ]    │
//   │   Grabaciones enviadas: 1    │
//   │   Siguiente: PAC-101 #2      │
//   │            [ OK ]            │
//   └──────────────────────────────┘
//
//   - [-]/[+] 로 원하는 환자 번호를 자유롭게 선택 (PAC-101 부터). 길게 누르면 빠르게 바뀜
//   - 환자마다 세션 번호가 따로 매겨짐: PAC-101 #1, #2 / PAC-102 #1 / ...
//     (ENVIAR 를 누를 때 그 환자의 번호가 +1)
//   - 선택한 환자와 각 환자의 세션 번호는 전원을 꺼도 유지됨
//   - GRABAR 를 누른 순간의 환자 ID 가 그 세션 데이터에 저장됨
constexpr int PACIENTE_MIN = 101;
constexpr int PACIENTE_MAX = 999;
constexpr lv_coord_t PAC_LABEL_X = 384;  // 화면에 ID 를 보여주는 라벨 위치 (PACIENTE 버튼 위)
constexpr lv_coord_t PAC_LABEL_Y = 132;

static Preferences pacPrefs;
static int pacienteNum = 0;            // 0 = 아직 선택 안 함
String pacienteID = "SIN_ID";          // FlutterFlow 로 보내는 ID
static lv_obj_t *pacLabel = NULL;
static lv_obj_t *pacPopup = NULL;
static lv_obj_t *pacPopupId = NULL;
static lv_obj_t *pacPopupInfo = NULL;
static int pacPopupNum = PACIENTE_MIN;  // 창에서 고르는 중인 번호

// 환자별 "보낸 세션 수" (NVS 키: "s101", "s102", ...)
static int pacienteSessionsSent(int num) {
    char k[8];
    snprintf(k, sizeof(k), "s%d", num);
    return pacPrefs.getInt(k, 0);
}

static int pacienteNextSession(int num) {
    char k[8];
    snprintf(k, sizeof(k), "s%d", num);
    int n = pacPrefs.getInt(k, 0) + 1;
    pacPrefs.putInt(k, n);
    return n;
}

static void updatePacienteID() {
    pacienteID = pacienteNum >= PACIENTE_MIN ? "PAC-" + String(pacienteNum) : String("SIN_ID");
    if (pacLabel != NULL) {
        lv_label_set_text(pacLabel, pacienteNum >= PACIENTE_MIN ? pacienteID.c_str() : "PAC-?");
    }
}

static void pacPopupRefresh() {
    char b[64];
    snprintf(b, sizeof(b), "PAC-%d", pacPopupNum);
    lv_label_set_text(pacPopupId, b);
    int sent = pacienteSessionsSent(pacPopupNum);
    snprintf(b, sizeof(b), "Grabaciones enviadas: %d\nSiguiente: PAC-%d #%d", sent, pacPopupNum, sent + 1);
    lv_label_set_text(pacPopupInfo, b);
}

static void pacStepEvent(lv_event_t *e) {
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    pacPopupNum = LV_CLAMP(PACIENTE_MIN, pacPopupNum + d, PACIENTE_MAX);
    pacPopupRefresh();
}

static void pacOkEvent(lv_event_t *e) {
    bool changed = pacPopupNum != pacienteNum;
    pacienteNum = pacPopupNum;
    pacPrefs.putInt("num", pacienteNum);
    updatePacienteID();
    lv_obj_add_flag(pacPopup, LV_OBJ_FLAG_HIDDEN);
    char b[48];
    snprintf(b, sizeof(b), LV_SYMBOL_OK " Paciente: %s", pacienteID.c_str());
    showToast(b, 1500, 0x1E5B8C);
    if (changed && sessionLogRecording()) {
        // 녹화 도중에 환자를 바꾸면: 새 환자 ID 로 처음부터 다시 녹화
        sessionLogStart(objetivoVal, pacienteID.c_str());
        showToast("Nuevo paciente: grabacion\nreiniciada", 2000, 0xC81E1E);
    }
}

static lv_obj_t *popupButton(lv_obj_t *parent, const char *txt, lv_coord_t w, lv_event_cb_t cb, void *ud) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, 48);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1E5B8C), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED_REPEAT, ud);  // 길게 누르면 반복
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return b;
}

static void createPacientePopup() {
    pacPopup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(pacPopup, 300, 220);
    lv_obj_center(pacPopup);
    lv_obj_set_style_bg_color(pacPopup, lv_color_hex(0x06243F), 0);
    lv_obj_set_style_bg_opa(pacPopup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pacPopup, lv_color_hex(0x6EC6FF), 0);
    lv_obj_set_style_border_width(pacPopup, 2, 0);
    lv_obj_set_style_radius(pacPopup, 16, 0);
    lv_obj_clear_flag(pacPopup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(pacPopup);
    lv_label_set_text(t, "Paciente");
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, -4);

    lv_obj_t *m = popupButton(pacPopup, LV_SYMBOL_MINUS, 56, pacStepEvent, (void *)(intptr_t)-1);
    lv_obj_align(m, LV_ALIGN_TOP_LEFT, 0, 24);
    lv_obj_t *p = popupButton(pacPopup, LV_SYMBOL_PLUS, 56, pacStepEvent, (void *)(intptr_t)1);
    lv_obj_align(p, LV_ALIGN_TOP_RIGHT, 0, 24);

    pacPopupId = lv_label_create(pacPopup);
    lv_obj_set_style_text_color(pacPopupId, lv_color_hex(0xFFE81A), 0);
    lv_obj_set_style_text_font(pacPopupId, &lv_font_montserrat_16, 0);
    lv_obj_align(pacPopupId, LV_ALIGN_TOP_MID, 0, 38);

    pacPopupInfo = lv_label_create(pacPopup);
    lv_obj_set_style_text_color(pacPopupInfo, lv_color_white(), 0);
    lv_obj_set_style_text_font(pacPopupInfo, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(pacPopupInfo, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(pacPopupInfo, LV_ALIGN_TOP_MID, 0, 84);

    lv_obj_t *ok = lv_btn_create(pacPopup);
    lv_obj_set_size(ok, 120, 44);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(ok, 12, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(0x1F8A3A), 0);
    lv_obj_add_event_cb(ok, pacOkEvent, LV_EVENT_CLICKED, NULL);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, "OK");
    lv_obj_set_style_text_font(okl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(okl, lv_color_white(), 0);
    lv_obj_center(okl);

    lv_obj_add_flag(pacPopup, LV_OBJ_FLAG_HIDDEN);
}

void ContarPaciente(lv_event_t * e) {
    if (pacPopup == NULL) return;
    pacPopupNum = pacienteNum >= PACIENTE_MIN ? pacienteNum : PACIENTE_MIN;
    pacPopupRefresh();
    lv_obj_clear_flag(pacPopup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(pacPopup);
}

static void createPacienteLabel() {
    pacPrefs.begin("paciente", false);
    pacienteNum = pacPrefs.getInt("num", 0);
    if (pacienteNum < PACIENTE_MIN || pacienteNum > PACIENTE_MAX) pacienteNum = 0;
    if (ui_MODO_REHABILITACION != NULL) {
        pacLabel = lv_label_create(ui_MODO_REHABILITACION);
        lv_obj_set_pos(pacLabel, PAC_LABEL_X, PAC_LABEL_Y);
        lv_obj_set_width(pacLabel, 90);
        lv_obj_set_style_text_align(pacLabel, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(pacLabel, lv_color_white(), 0);
        lv_obj_set_style_text_font(pacLabel, &lv_font_montserrat_14, 0);
        lv_obj_set_style_bg_color(pacLabel, lv_color_hex(0x06243F), 0);
        lv_obj_set_style_bg_opa(pacLabel, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pacLabel, 8, 0);
        lv_obj_set_style_pad_ver(pacLabel, 2, 0);
        lv_obj_clear_flag(pacLabel, LV_OBJ_FLAG_CLICKABLE);
    }
    createPacientePopup();
    updatePacienteID();
}
// ==========================================

static uint32_t grabarMillis = 0;
static uint32_t lastFsResult = 0;

void GrabarSession(lv_event_t * e) {
    // 버튼 위젯을 기억 (SquareLine 위젯 이름을 몰라도 동작하도록)
    if (grabarBtn == NULL && e != NULL) {
        grabarBtn = lv_event_get_target(e);
        grabarLabel = findChildLabel(grabarBtn);
        if (grabarLabel != NULL) {
            strncpy(grabarOrigText, lv_label_get_text(grabarLabel), sizeof(grabarOrigText) - 1);
        }
        grabarOrigColor = lv_obj_get_style_bg_color(grabarBtn, LV_PART_MAIN);
    }
    sessionLogStart(objetivoVal, pacienteID.c_str());
    if (isGamePaused) sessionLogPause(true);
    grabarMillis = millis();   // "fecha" en Firestore = este momento
    if (USAR_BLUETOOTH_APP) bleAppSendEstado();
    char b[80];
    snprintf(b, sizeof(b), "Grabando: %s", pacienteID.c_str());
    showToast(b, 2000, 0xC81E1E);
}

void EnviarSession(lv_event_t * e) {
    if (firestoreBusy()) {
        showToast("Enviando...\nespera un momento", 2000, 0x1E5AA8);
        return;
    }
    if (!sessionLogRecording()) {
        // El envio anterior fallo (sin hotspot, etc.): reintentar
        if (firestoreHasUnsent() && firestoreRetry()) {
            showToast(LV_SYMBOL_REFRESH " Reintentando envio...", 15000, 0x1E5AA8);
            return;
        }
        showToast("No hay datos.\nPrimero presiona GRABAR", 2500, 0x8C5A1E);
        return;
    }
    // 이 환자의 다음 세션 번호 (PAC-101 #1, #2, ...)
    int nSes = pacienteNum >= PACIENTE_MIN ? pacienteNextSession(pacienteNum) : 0;
    sessionLogSend(objetivoVal, progresoVal, sessionCompleted, nSes);
    if (USAR_BLUETOOTH_APP) bleAppSendSession();
    char b[110];
    if (firestoreStartUpload(pacienteID.c_str(), nSes, grabarMillis)) {
        // el resultado (OK / error) aparece solo cuando termina (ver loop)
        snprintf(b, sizeof(b), LV_SYMBOL_UPLOAD " Enviando %s #%d\na la app...", pacienteID.c_str(), nSes);
        showToast(b, 15000, 0x1E5AA8);
    } else {
        showToast(LV_SYMBOL_WARNING " No se pudo enviar", 2500, 0xC81E1E);
    }
}

// Muestra el resultado del envio a Firestore cuando termina (lo llama loop)
static void firestoreResultUpdate() {
    uint32_t id = firestoreResultId();
    if (id == lastFsResult) return;
    lastFsResult = id;
    FirestoreState st = firestoreState();
    showToast(firestoreMessage(), st == FS_OK ? 2500 : 4000, st == FS_OK ? 0x1F8A3A : 0xC81E1E);
}
// ==========================================


// ==========================================
// 요약 화면(RESUMEN DE SESSION)
// MODO JUEGO에서 TERMINAR4를 눌렀을 때 실행
void prepareSummaryForJuego(lv_event_t * e) {
    ultimoModo = 1; // 게임 모드에서 왔음을 기억

    if (ui_lblSummaryTarget != NULL) {
        lv_label_set_text(ui_lblSummaryTarget, "-");
    }
    if (ui_lblSummaryAchieved != NULL) {
        char ptsStr[10];
        sprintf(ptsStr, "%d", puntos);
        lv_label_set_text(ui_lblSummaryAchieved, ptsStr);
    }
}

// MODO REHABILITACION에서 TERMINAR5를 눌렀을 때 실행
void prepareSummaryForRehab(lv_event_t * e) {
    ultimoModo = 2; // 재활 모드에서 왔음을 기억
    updateRehabUI();
}

// REPETIR 버튼을 눌렀을 때 실행될 동적 화면 전환 함수
void RepetirSession(lv_event_t * e) {
    if (ultimoModo == 1) {
        // 게임 모드에서 왔었다면 다시 게임 모드로
        _ui_screen_change(&ui_MODO_JUEGO, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, &ui_MODO_JUEGO_screen_init);
    } else {
        // 재활 모드에서 왔었다면 다시 재활 모드로
        _ui_screen_change(&ui_MODO_REHABILITACION, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, &ui_MODO_REHABILITACION_screen_init);
    }
}
// ==========================================


// ==========================================
// 요약 화면 칭찬 음성 (RESUMEN DE SESSION 에 들어가면 한 번 말함)
//   목표 달성 (재활: 목표 횟수 채움 / 게임: 1점 이상):
//       ¡Bravo! · ¡Excelente! · ¡Lo lograste!
//   목표 미달 (그래도 전부 긍정적인 말):
//       ¡Buen trabajo! · ¡Muy bien, sigue así! · ¡Lo hiciste genial!
//   각 그룹에서 무작위로 고르고, 바로 전에 나온 말은 다시 나오지 않음.
constexpr bool RESUMEN_REACCION = true;           // false 로 하면 칭찬 음성 끔
constexpr uint32_t RESUMEN_VOICE_DELAY_MS = 150;  // 화면 전환이 끝난 뒤 기다리는 시간 (ms)

static const Sfx REACCION_LOGRO[] = {SFX_BRAVO, SFX_EXCELENTE, SFX_LO_LOGRASTE};
static const Sfx REACCION_ANIMO[] = {SFX_BUEN_TRABAJO, SFX_MUY_BIEN, SFX_LO_HICISTE_GENIAL};
static int ultimaReaccion = -1;   // 직전에 나온 음성 (연속 반복 방지)

static void resumenReaccionTimerCb(lv_timer_t * t) {
    if (lv_scr_act() != ui_RESUMEN_DE_SESSION) return;   // 그 사이에 화면을 나갔으면 말하지 않음
    bool logro = (ultimoModo == 2) ? (progresoVal >= objetivoVal) : (puntos > 0);
    const Sfx *lista = logro ? REACCION_LOGRO : REACCION_ANIMO;
    const int n = 3;
    int i = random(n);
    if ((int)lista[i] == ultimaReaccion) i = (i + 1 + random(n - 1)) % n;  // 같은 말 연속 금지
    ultimaReaccion = (int)lista[i];
    audioPlaySfx(lista[i]);
    Serial.printf("[RESUMEN] reaccion %s #%d\n", logro ? "LOGRO" : "ANIMO", i);
}

// RESUMEN DE SESSION 화면 전환이 완전히 끝났을 때 (LV_EVENT_SCREEN_LOADED)
static void on_Resumen_Screen_Loaded(lv_event_t * e) {
    if (!RESUMEN_REACCION) return;
    lv_timer_t * t = lv_timer_create(resumenReaccionTimerCb, RESUMEN_VOICE_DELAY_MS, NULL);
    lv_timer_set_repeat_count(t, 1);  // 한 번만 실행되고 자동 삭제
}
// ==========================================

// ==========================================
// 화면 진입 시 값을 기본값으로 되돌리는 함수
// SELECCION DE MODO → MODO JUEGO 로 넘어갈 때만:
// 화면 전환(FADE 300 ms)이 완전히 끝난 뒤에 색상 이름을 말함.
// (RESUMEN DE SESSION 의 REPETIR 로 돌아올 때는 기존처럼 바로 말함)
constexpr uint32_t JUEGO_VOICE_DELAY_MS = 100;  // 전환이 끝난 뒤 추가로 기다리는 시간 (ms)
static bool speakAfterJuegoLoaded = false;

static void juegoVoiceTimerCb(lv_timer_t * t) {
    // 그 사이에 다른 화면으로 나갔다면 말하지 않음
    if (lv_scr_act() == ui_MODO_JUEGO) {
        speakCurrentColor();
    }
}

// MODO JUEGO 화면에 들어올 때 (전환 시작 시점, LV_EVENT_SCREEN_LOAD_START)
void on_Juego_Screen_Load(lv_event_t * e) {
    // LVGL 8.3: 전환 애니메이션이 시작될 때 prev_scr 에 이전 화면이 들어 있음
    lv_disp_t * disp = lv_disp_get_default();
    bool fromSeleccion = (disp != NULL && disp->prev_scr == ui_SELECCION_DE_MODO);

    ultimoModo = 1;          // 진입하자마자 게임 모드로 기억
    puntos = 0;              // 점수를 0으로 초기화
    updatePuntosUI();        // 화면 갱신

    if (fromSeleccion) {
        generateRandomColor(false);     // 글자만 먼저 바꾸고, 음성은 전환이 끝난 뒤에
        speakAfterJuegoLoaded = true;
    } else {
        speakAfterJuegoLoaded = false;
        generateRandomColor();          // 기존과 동일 (색상 + 음성 바로)
    }
}

// MODO JUEGO 화면 전환이 완전히 끝났을 때 (LV_EVENT_SCREEN_LOADED)
void on_Juego_Screen_Loaded(lv_event_t * e) {
    if (!speakAfterJuegoLoaded) return;
    speakAfterJuegoLoaded = false;

    if (JUEGO_VOICE_DELAY_MS == 0) {
        speakCurrentColor();
    } else {
        lv_timer_t * t = lv_timer_create(juegoVoiceTimerCb, JUEGO_VOICE_DELAY_MS, NULL);
        lv_timer_set_repeat_count(t, 1);  // 한 번만 실행되고 자동 삭제
    }
}

// MODO REHABILITACION 화면에 들어올 때
void on_Rehab_Screen_Load(lv_event_t * e) {
    ultimoModo = 2;          // 진입하자마자 재활 모드로 기억
    objetivoVal = 1;         // 기본 목표값 1
    progresoVal = 0;         // 달성 횟수 0으로 초기화
    sessionCompleted = false;
    pendingSummary = false;
    isGamePaused = false;
    if (pausarLabel != NULL) lv_label_set_text(pausarLabel, pausarOrigText);  // 버튼 글자 PAUSAR 로
    updateRehabUI();         // 화면 갱신
}
// ==========================================


// ==========================================
// 위/아래 화살표 (발표용 수동 조작): 누르고 있는 동안만 거북이가 전진/후진
//
// 버튼은 코드로 만들어짐 (SquareLine 수정 불필요): createFlechas()
// 위치는 아래 FLECHAS_* 상수로 조정.
//
// 손을 떼거나, 손가락이 버튼 밖으로 미끄러지거나, 화면을 나가면 바로 정지.
// 비상정지·REGRESAR 가 우선. EMG 반복 횟수에는 포함되지 않음.
// ※ Stage 3 평가에서는 "EMG 신호가 출력을 움직여야" 하므로 공식 시연에서는 쓰지 말 것.
constexpr bool FLECHAS_ACTIVAS = true;   // false = 화살표 무시
static int8_t manualDir = 0;             // +1 전진, -1 후진, 0 없음
static lv_obj_t *manualBtn = NULL;

static void startManual(lv_event_t *e, int8_t dir) {
    if (!FLECHAS_ACTIVAS) return;
    manualDir = dir;
    manualBtn = e != NULL ? lv_event_get_target(e) : NULL;
}

void FlechaArriba(lv_event_t * e) { startManual(e, 1); }
void FlechaAbajo(lv_event_t * e)  { startManual(e, -1); }

void FlechaSoltar(lv_event_t * e) {
    // 여기서는 표시만 하고, STOP 전송은 loop 의 turtleUpdate() 가 바로 다음에 함
    // (터치 이벤트 안에서 무선 전송을 하지 않음)
    manualDir = 0;
    manualBtn = NULL;
}

// ---- 화살표 버튼 만들기 ----
// 위치 (왼쪽 위 기준). 버튼 2개가 세로로 놓임: ▲ 위, ▼ 아래
constexpr lv_coord_t FLECHAS_JUEGO_X = 400;   // MODO JUEGO: PUNTOS 아래, REGRESAR 위
constexpr lv_coord_t FLECHAS_JUEGO_Y = 110;
constexpr lv_coord_t FLECHAS_REHAB_X = 12;    // MODO REHABILITACION: 왼쪽 가운데
constexpr lv_coord_t FLECHAS_REHAB_Y = 145;
constexpr lv_coord_t FLECHA_W = 56;
constexpr lv_coord_t FLECHA_H = 44;
constexpr lv_coord_t FLECHA_GAP = 6;

static void flechaPressEvent(lv_event_t *e) {
    startManual(e, (int8_t)(intptr_t)lv_event_get_user_data(e));
}

static void flechaReleaseEvent(lv_event_t *e) {
    FlechaSoltar(e);
}

static void createFlechaBtn(lv_obj_t *scr, lv_coord_t x, lv_coord_t y, const char *sym, int8_t dir) {
    lv_obj_t *b = lv_btn_create(scr);
    lv_obj_set_size(b, FLECHA_W, FLECHA_H);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x06243F), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1E5B8C), LV_STATE_PRESSED);   // 누르면 밝아짐
    lv_obj_set_style_border_color(b, lv_color_hex(0x6EC6FF), 0);              // 다른 버튼과 같은 네온
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, flechaPressEvent, LV_EVENT_PRESSED, (void *)(intptr_t)dir);
    lv_obj_add_event_cb(b, flechaReleaseEvent, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(b, flechaReleaseEvent, LV_EVENT_PRESS_LOST, NULL);    // 손가락이 밖으로 나가도 정지
    lv_obj_add_event_cb(b, bubbleEvent, LV_EVENT_PRESSED, NULL);              // 버튼 소리
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_center(l);
}

static void createFlechas() {
    if (!FLECHAS_ACTIVAS) return;
    if (ui_MODO_JUEGO != NULL) {
        createFlechaBtn(ui_MODO_JUEGO, FLECHAS_JUEGO_X, FLECHAS_JUEGO_Y, LV_SYMBOL_UP, 1);
        createFlechaBtn(ui_MODO_JUEGO, FLECHAS_JUEGO_X, FLECHAS_JUEGO_Y + FLECHA_H + FLECHA_GAP, LV_SYMBOL_DOWN, -1);
    }
    if (ui_MODO_REHABILITACION != NULL) {
        createFlechaBtn(ui_MODO_REHABILITACION, FLECHAS_REHAB_X, FLECHAS_REHAB_Y, LV_SYMBOL_UP, 1);
        createFlechaBtn(ui_MODO_REHABILITACION, FLECHAS_REHAB_X, FLECHAS_REHAB_Y + FLECHA_H + FLECHA_GAP, LV_SYMBOL_DOWN, -1);
    }
}

// 버튼을 실제로 누르고 있고, 게임/재활 화면일 때만 true
static bool manualActive(lv_obj_t *scr) {
    if (manualDir == 0) return false;
    bool ok = (scr == ui_MODO_JUEGO || scr == ui_MODO_REHABILITACION) && manualBtn != NULL &&
              lv_obj_has_state(manualBtn, LV_STATE_PRESSED);
    if (!ok) {   // Released 이벤트를 놓쳐도 여기서 정지
        manualDir = 0;
        manualBtn = NULL;
    }
    return ok;
}
// ==========================================


// ==========================================
// EMG → 거북이 (블록 다이어그램: 임계값을 넘는 동안 전진, 활성화가 끝나면 정지)

// 비상정지: 버튼을 누르면 "잠김(latch)" 상태가 되고, 버튼을 놓은 뒤
// 화면의 빨간 안내창을 터치해야 해제됨 (실수로 바로 다시 움직이지 않도록)
static bool estopLatched = false;
static lv_obj_t *estopBanner = NULL;

static bool estopPressed() {
    return ESTOP_PIN >= 0 && digitalRead(ESTOP_PIN) == ESTOP_ACTIVE_LEVEL;
}

static void estopBannerEvent(lv_event_t *e) {
    if (estopPressed()) return;  // 버튼이 아직 눌려 있으면 해제 불가
    estopLatched = false;
    lv_obj_add_flag(estopBanner, LV_OBJ_FLAG_HIDDEN);
    Serial.println("[PARO] Liberado");
}

static void createEstopBanner() {
    estopBanner = lv_obj_create(lv_layer_top());
    lv_obj_set_size(estopBanner, 400, 120);
    lv_obj_center(estopBanner);
    lv_obj_set_style_bg_color(estopBanner, lv_color_hex(0xC81E1E), 0);
    lv_obj_set_style_bg_opa(estopBanner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(estopBanner, lv_color_white(), 0);
    lv_obj_set_style_border_width(estopBanner, 3, 0);
    lv_obj_set_style_radius(estopBanner, 16, 0);
    lv_obj_clear_flag(estopBanner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(estopBanner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(estopBanner, estopBannerEvent, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(estopBanner);
    lv_label_set_text(l, LV_SYMBOL_WARNING " PARO DE EMERGENCIA\n\nSuelta el boton y toca aqui\npara continuar");
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_center(l);
    lv_obj_add_flag(estopBanner, LV_OBJ_FLAG_HIDDEN);
}

static void estopUpdate() {
    if (ESTOP_PIN < 0) return;
    if (estopPressed() && !estopLatched) {
        estopLatched = true;
        sendMotor(MOTOR_CMD_STOP, 0);
        if (estopBanner != NULL) {
            lv_obj_clear_flag(estopBanner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(estopBanner);
        }
        Serial.println("[PARO] ACTIVADO: tortuga detenida");
    }
}

static bool estopActive() {
    return estopLatched || estopPressed();
}

// 지금 EMG 로 거북이를 움직여도 되는지
static bool emgDriveAllowed(lv_obj_t *scr) {
    if (estopActive()) return false;
    if (reversing) return false;
    EmgCalState cs = emgCalibrationState();
    if (cs == CAL_REST || cs == CAL_SQUEEZE) return false;  // 보정 중에는 움직이지 않음
    if (scr == ui_MODO_JUEGO) return true;
    if (scr == ui_MODO_REHABILITACION) return !isGamePaused;
    return false;  // 다른 화면에서는 절대 움직이지 않음
}

// 시험 기록 (Stage 3 결과표용). Serial 에 CSV 로 출력
static uint32_t trialNum = 0;
static uint32_t trialOnsetMs = 0;
static int32_t trialLatencyMs = -1;
static bool trialDrove = false;
static bool trialCounted = false;   // 이 활성화로 재활 횟수가 올라갔는지
static const char *trialBlockReason = "";

static const char *modeName(lv_obj_t *scr) {
    if (scr == ui_MODO_JUEGO) return "JUEGO";
    if (scr == ui_MODO_REHABILITACION) return "REHAB";
    return "OTRA";
}

// MODO JUEGO 의 전진 속도: RMS 가 클수록 빠름 (임계값 → MIN, 최대 힘 → MAX)
static uint8_t juegoSpeed(float rmsMv) {
    float thr = emgThreshold();
    float top = emgMvcMv() * JUEGO_MVC_FRACCION;          // CAL 로 측정한 최대 수축 기준
    if (top < thr * 1.5f) top = thr * JUEGO_SIN_CAL_FACTOR; // CAL 안 했거나 값이 너무 작으면
    float x = (rmsMv - thr) / (top - thr);
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    return (uint8_t)lroundf(JUEGO_SPEED_MIN + x * (JUEGO_SPEED_MAX - JUEGO_SPEED_MIN));
}

static void turtleUpdate() {
    static bool wasActive = false;
    lv_obj_t *scr = lv_scr_act();
    uint32_t now = millis();
    EmgSnapshot s = emgRead();

    // --- 활성화 시작 ---
    if (s.active && !wasActive) {
        trialNum++;
        trialOnsetMs = s.onsetMs;
        trialLatencyMs = -1;
        trialDrove = false;
        emgTakePeakMv();  // 피크 측정 초기화
        bool allowed = emgDriveAllowed(scr);
        trialBlockReason = allowed ? "" : estopActive() ? "PARO" : reversing ? "REGRESAR"
                         : (emgCalibrationState() == CAL_REST || emgCalibrationState() == CAL_SQUEEZE) ? "CALIBRANDO"
                         : (scr == ui_MODO_REHABILITACION && isGamePaused) ? "PAUSA" : "OTRA_PANTALLA";
        trialCounted = false;
        if (allowed && scr == ui_MODO_REHABILITACION) {
            trialCounted = countRehabRep();
        }
    }

    // --- 거북이 명령 ---
    if (estopActive()) {
        // 비상정지: REGRESAR 도 취소하고 STOP 만 보냄
        reversing = false;
        if (lastSentCmd != MOTOR_CMD_STOP || now - lastSendMs >= MOTOR_RESEND_MS) {
            sendMotor(MOTOR_CMD_STOP, 0);
        }
    } else if (reversing) {
        if ((int32_t)(now - reverseUntilMs) >= 0) {
            reversing = false;
            sendMotor(MOTOR_CMD_STOP, 0);
        } else if (lastSentCmd != MOTOR_CMD_REVERSE || now - lastSendMs >= MOTOR_RESEND_MS) {
            sendMotor(MOTOR_CMD_REVERSE, TURTLE_SPEED);
        }
    } else if (manualActive(scr)) {
        // 화살표: 누르고 있는 동안 100 ms 마다 명령 (EMG 보다 우선)
        uint8_t cmd = manualDir > 0 ? MOTOR_CMD_FORWARD : MOTOR_CMD_REVERSE;
        if (lastSentCmd != cmd || now - lastSendMs >= MOTOR_RESEND_MS) {
            sendMotor(cmd, TURTLE_SPEED);
        }
    } else if (s.active && emgDriveAllowed(scr)) {
        // 수축하는 동안 100 ms 마다 전진 명령 (거북이는 300 ms 동안 못 받으면 스스로 정지)
        // MODO JUEGO: 근육 활성도에 비례하는 속도 / 그 외: 고정 속도
        uint8_t speed = (JUEGO_VELOCIDAD_VARIABLE && scr == ui_MODO_JUEGO) ? juegoSpeed(s.rmsMv) : TURTLE_SPEED;
        int dSpeed = (int)speed - (int)lastSentSpeed;
        bool speedChanged = lastSentCmd == MOTOR_CMD_FORWARD && (dSpeed >= JUEGO_SPEED_STEP || dSpeed <= -JUEGO_SPEED_STEP);
        if (lastSentCmd != MOTOR_CMD_FORWARD || speedChanged || now - lastSendMs >= MOTOR_RESEND_MS) {
            sendMotor(MOTOR_CMD_FORWARD, speed);
            if (!trialDrove) {
                trialDrove = true;
                trialLatencyMs = (int32_t)(millis() - trialOnsetMs);
            }
        }
    } else if (lastSentCmd != MOTOR_CMD_STOP) {
        sendMotor(MOTOR_CMD_STOP, 0);
    } else if (now - lastSendMs >= (scr == ui_CONEXION_BLUETOOTH ? 300u : 1000u)) {
        // 정지 상태에서 주기적으로 STOP 을 보내 연결 상태(ACK)를 확인
        sendMotor(MOTOR_CMD_STOP, 0);
    }

    // --- 활성화 끝: 결과 한 줄 기록 ---
    if (!s.active && wasActive) {
        uint32_t durMs = now - trialOnsetMs;
        float peak = emgTakePeakMv();
        // TRIAL,n,modo,t_s,latencia_ms,duracion_ms,pico_rms_mV,pico_uV_electrodo,umbral_mV,salida,conteo,enlace
        Serial.printf("TRIAL,%lu,%s,%.1f,%ld,%lu,%.1f,%.0f,%.1f,%s,%d/%d,%s\n",
                      (unsigned long)trialNum, modeName(scr), trialOnsetMs / 1000.0,
                      (long)trialLatencyMs, (unsigned long)durMs, (double)peak,
                      (double)(peak * 1000.0f / EMG_TOTAL_GAIN), (double)emgThreshold(),
                      trialDrove ? "AVANZA" : trialBlockReason, progresoVal, objetivoVal,
                      turtleLinkOk() ? "OK" : "SIN_ACK");

        if (trialCounted) {
            sessionLogRep(trialOnsetMs, durMs, peak, trialLatencyMs, false);  // GRABAR 중이면 기록
        }

        if (pendingSummary) {
            pendingSummary = false;
            _ui_screen_change(&ui_RESUMEN_DE_SESSION, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, &ui_RESUMEN_DE_SESSION_screen_init);
        }
    }
    wasActive = s.active;

    // 화면을 나가면 보정 취소
    if (scr != ui_MODO_JUEGO && scr != ui_MODO_REHABILITACION) {
        emgCancelCalibration();
        reversing = false;
        pendingSummary = false;
    }

    // 연결 상태 변화 출력
    static bool lastLink = false;
    bool link = turtleLinkOk();
    if (link != lastLink) {
        lastLink = link;
        Serial.printf("[ESP-NOW] Tortuga %s\n", link ? "CONECTADA" : "SIN RESPUESTA");
    }
}
// ==========================================


// ==========================================
// CONEXION BLUETOOTH 화면: 연결 상태 패널 (코드로 생성, SquareLine 수정 불필요)
//
//   ● Pantalla   OK
//   ● Tortuga    conectada / sin conexion
//   ● EMG        senal OK / revisar cables
//
// 세 가지가 모두 OK 일 때만 자동으로 SELECCION DE MODO 로 넘어감.
// 위치는 아래 상수로 조정하거나, 손가락으로 끌어서 옮기면 저장됨.
constexpr lv_coord_t CONN_PANEL_X = 245;
constexpr lv_coord_t CONN_PANEL_Y = 95;

static lv_obj_t *connPanel = NULL;
static lv_obj_t *connLblTurtle = NULL;
static lv_obj_t *connLblEmg = NULL;
static lv_obj_t *connLblMsg = NULL;
static lv_obj_t *connLblApp = NULL;
static Preferences connPrefs;
static bool connDragged = false;

// EMG 회로가 연결되어 있는지 (근사): 입력의 DC 레벨이 0 V / 3.1 V 에 붙어 있지 않고 포화도 없음.
// 핀이 완전히 떠 있는 경우까지 100% 구분하지는 못함 (한계).
static bool emgSignalOk() {
    if (EMG_REMOTE && !emgRemoteLinkOk()) return false;   // no llegan datos del ESP32 EMG
    EmgSnapshot s = emgRead();
    return s.sampleCount > 0 && !s.saturated && s.dcMv > 100.0f && s.dcMv < 3000.0f;
}

static bool allConnected() {
    return turtleLinkOk() && emgSignalOk();
}

static void connPanelEvent(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        connDragged = false;
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev == NULL) return;
        lv_point_t v;
        lv_indev_get_vect(indev, &v);
        if (v.x == 0 && v.y == 0) return;
        lv_coord_t x = LV_CLAMP(0, lv_obj_get_x(connPanel) + v.x, 480 - lv_obj_get_width(connPanel));
        lv_coord_t y = LV_CLAMP(0, lv_obj_get_y(connPanel) + v.y, 320 - lv_obj_get_height(connPanel));
        lv_obj_set_pos(connPanel, x, y);
        connDragged = true;
    } else if (code == LV_EVENT_RELEASED && connDragged) {
        connPrefs.putShort("x", lv_obj_get_x(connPanel));
        connPrefs.putShort("y", lv_obj_get_y(connPanel));
    }
}

static lv_obj_t *connRow(const char *text, lv_coord_t y) {
    lv_obj_t *l = lv_label_create(connPanel);
    lv_label_set_recolor(l, true);
    lv_obj_set_pos(l, 0, y);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_label_set_text(l, text);
    return l;
}

static void createConnPanel() {
    if (ui_CONEXION_BLUETOOTH == NULL) return;
    connPrefs.begin("connui", false);
    connPanel = lv_obj_create(ui_CONEXION_BLUETOOTH);
    lv_obj_set_size(connPanel, 230, 124);
    lv_coord_t x = connPrefs.getShort("x", CONN_PANEL_X);
    lv_coord_t y = connPrefs.getShort("y", CONN_PANEL_Y);
    if (x < 0 || x > 250 || y < 0 || y > 196) { x = CONN_PANEL_X; y = CONN_PANEL_Y; }
    lv_obj_set_pos(connPanel, x, y);
    lv_obj_set_style_pad_all(connPanel, 8, 0);
    lv_obj_set_style_radius(connPanel, 12, 0);
    lv_obj_set_style_bg_color(connPanel, lv_color_hex(0x06243F), 0);
    lv_obj_set_style_bg_opa(connPanel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(connPanel, lv_color_hex(0x6EC6FF), 0);
    lv_obj_set_style_border_width(connPanel, 1, 0);
    lv_obj_clear_flag(connPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(connPanel, connPanelEvent, LV_EVENT_ALL, NULL);

    connRow("#3CCB5A " LV_SYMBOL_OK "# Pantalla: OK", 0);
    connLblTurtle = connRow("Tortuga: ...", 22);
    connLblEmg = connRow("EMG: ...", 44);
    connLblApp = connRow("App: ...", 66);
    connLblMsg = connRow("", 88);
}

static void updateConnPanel() {
    if (connPanel == NULL || lv_scr_act() != ui_CONEXION_BLUETOOTH) return;
    static uint32_t last = 0;
    if (millis() - last < 250) return;
    last = millis();

    bool t = turtleLinkOk();
    bool m = emgSignalOk();
    lv_label_set_text(connLblTurtle, t ? "#3CCB5A " LV_SYMBOL_OK "# Tortuga: conectada"
                                       : "#FF6B6B " LV_SYMBOL_CLOSE "# Tortuga: sin conexion");
    if (m) {
        lv_label_set_text(connLblEmg, "#3CCB5A " LV_SYMBOL_OK "# EMG: senal OK");
    } else if (EMG_REMOTE && !emgRemoteLinkOk()) {
        lv_label_set_text(connLblEmg, "#FF6B6B " LV_SYMBOL_CLOSE "# EMG: ESP32 sin conexion");
    } else {
        lv_label_set_text(connLblEmg, "#FF6B6B " LV_SYMBOL_CLOSE "# EMG: revisar cables");
    }
    // La app no es obligatoria para jugar (solo para ENVIAR), por eso no entra en "Todo listo"
    if (USAR_BLUETOOTH_APP) {
        lv_label_set_text(connLblApp, bleAppConnected() ? "#3CCB5A " LV_SYMBOL_BLUETOOTH "# App: conectada"
                                                        : "#FFE81A " LV_SYMBOL_BLUETOOTH "# App: esperando...");
    } else if (firestoreBusy()) {
        lv_label_set_text(connLblApp, "#6EC6FF " LV_SYMBOL_WIFI "# App: conectando...");
    } else if (firestoreHasUnsent()) {
        lv_label_set_text(connLblApp, "#FFE81A " LV_SYMBOL_WIFI "# App: envio pendiente");
    } else if (firestoreState() == FS_OK) {
        lv_label_set_text(connLblApp, "#3CCB5A " LV_SYMBOL_WIFI "# App: ultimo envio OK");
    } else {
        lv_label_set_text(connLblApp, sessionLogClockValid() ? "#3CCB5A " LV_SYMBOL_WIFI "# App: hora OK (hotspot)"
                                                             : "#FFE81A " LV_SYMBOL_WIFI "# App: se conecta al ENVIAR");
    }
    lv_label_set_text(connLblMsg, (t && m) ? "#3CCB5A Todo listo#" : "#FFE81A Revisa lo marcado en rojo#");
}

// (선택) SquareLine 에서 CONECTAR 버튼의 Action 을 "Change screen" 대신
// "Call function: Conectar" 로 바꾸면, 연결이 안 됐을 때 모드 선택으로 못 넘어가게 막음.
void Conectar(lv_event_t * e) {
    if (allConnected()) {
        _ui_screen_change(&ui_SELECCION_DE_MODO, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, &ui_SELECCION_DE_MODO_screen_init);
    } else {
        showToast(LV_SYMBOL_WARNING " No conectado\nRevisa tortuga y EMG", 2500, 0xC81E1E);
    }
}
// ==========================================


void setup() {
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
    if (ESPEJO_USB) Serial.setTxBufferSize(ESPEJO_TX_BUFFER);   // antes de begin (por defecto 256)
#endif
    Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
    // USB 로 노트북에 연결돼 있는데 Serial Monitor 가 안 열려 있으면, Serial 출력이
    // 쌓여서 print 할 때마다 최대 100 ms 씩 기다림 → 화면이 멈춘 것처럼 됨.
    // 0 = 기다리지 않고 버림 (화면이 절대 Serial 때문에 멈추지 않음)
    Serial.setTxTimeoutMs(0);
#endif
    audioBegin();
    if (audioGetVolume() > VOL_REAL_MAX) {   // volumen guardado de antes (hasta 100 %): bajarlo al limite
        audioSetVolume(VOL_REAL_MAX);
        audioSaveVolume();
    }

    if (ESTOP_PIN >= 0) pinMode(ESTOP_PIN, INPUT_PULLUP);
    if (BALLENA_BAT_PIN >= 0) analogSetPinAttenuation(BALLENA_BAT_PIN, ADC_11db);   // hasta ~3.1 V en el pin

    // --- ESP-NOW 설정 ---
    // (실패해도 return 하지 않음: 화면은 계속 동작해야 함)
    // STA = ESP-NOW (tortuga y ESP32 EMG). AP = red Wi-Fi propia para la app (solo si SESSION_WIFI_AP)
    WiFi.mode(SESSION_WIFI_AP ? WIFI_AP_STA : WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE); // mismo canal que el XIAO
    if (esp_now_init() != ESP_OK) {
        Serial.println("Error initializing ESP-NOW");
    } else {
        esp_now_register_send_cb(onEspNowSent);
        esp_now_register_recv_cb(onEspNowRecv);   // bateria de la tortuga
        memcpy(peerInfo.peer_addr, turtleAddress, 6);
        peerInfo.channel = ESPNOW_CHANNEL;
        peerInfo.encrypt = false;
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("Failed to add peer");
        }
    }
    // --- ESP-NOW 설정 끝 ---

    // Red Wi-Fi propia + servidor de datos (mismo canal que ESP-NOW)
    sessionLogBegin();

    // EMG: modo remoto = recibe del ESP32 EMG por ESP-NOW (ver emg.h, EMG_REMOTE)
    emgBegin();
    Serial.printf("[PANTALLA] MAC: %s  canal ESP-NOW %u\n", WiFi.macAddress().c_str(), ESPNOW_CHANNEL);

    // App: ENVIAR -> Firestore por el hotspot (y toma la hora al encender)
    firestoreBegin();
    if (USAR_BLUETOOTH_APP) bleAppBegin();

    // 백라이트 핀 켜기 (GPIO 45)
    pinMode(45, OUTPUT);
    digitalWrite(45, HIGH);

    // LovyanGFX 및 LVGL 초기화
    tft.begin();
    tft.setSwapBytes(true);
    tft.setRotation(1);   // 가로 모드 (480x320)

    lv_init();

    // buffers de dibujo en RAM interna con DMA
    const size_t bufBytes = screenWidth * DRAW_BUF_LINES * sizeof(lv_color_t);
    drawBuf1 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (DISPLAY_DMA) drawBuf2 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (drawBuf1 == NULL) {   // no deberia pasar: buffer pequeno de emergencia
        static lv_color_t fallbackBuf[screenWidth * 10];
        drawBuf1 = fallbackBuf;
        lv_disp_draw_buf_init(&draw_buf, drawBuf1, NULL, screenWidth * 10);
    } else {
        lv_disp_draw_buf_init(&draw_buf, drawBuf1, drawBuf2, screenWidth * DRAW_BUF_LINES);
    }
    Serial.printf("[PANTALLA] buffers: %s, RAM interna libre %u bytes\n", drawBuf2 ? "2 + DMA" : "1",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (ESPEJO_USB) {   // copia de la pantalla para el espejo USB: en PSRAM, no gasta RAM interna
        espejoFb = (uint16_t *)heap_caps_malloc(screenWidth * screenHeight * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        Serial.println(espejoFb ? "[ESPEJO] listo: abrir tools/espejo.html en la computadora"
                                : "[ESPEJO] sin PSRAM: espejo apagado");
    }

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    // SquareLine UI 로드
    ui_init();
    addBubbleToButtons();
    emgPanelCreate();   // EMG 바/그래프 (MODO JUEGO / REHABILITACION 에서만 보임)

    // TERMINAR 버튼 클릭 시 요약 화면 내용 업데이트
    if (ui_TERMINAR4 != NULL) {
        lv_obj_add_event_cb(ui_TERMINAR4, prepareSummaryForJuego, LV_EVENT_CLICKED, NULL);
    }
    if (ui_TERMINAR5 != NULL) {
        lv_obj_add_event_cb(ui_TERMINAR5, prepareSummaryForRehab, LV_EVENT_CLICKED, NULL);
    }

    // 화면(Screen) 진입 시 초기화
    if (ui_MODO_JUEGO != NULL) {
        lv_obj_add_event_cb(ui_MODO_JUEGO, on_Juego_Screen_Load, LV_EVENT_SCREEN_LOAD_START, NULL);
        // 화면 전환이 끝난 뒤 (SELECCION DE MODO 에서 왔을 때만 색상 음성 재생)
        lv_obj_add_event_cb(ui_MODO_JUEGO, on_Juego_Screen_Loaded, LV_EVENT_SCREEN_LOADED, NULL);
    }
    if (ui_MODO_REHABILITACION != NULL) {
        lv_obj_add_event_cb(ui_MODO_REHABILITACION, on_Rehab_Screen_Load, LV_EVENT_SCREEN_LOAD_START, NULL);
    }
    if (ui_RESUMEN_DE_SESSION != NULL) {
        // 요약 화면: 결과에 맞는 칭찬 음성 (전환이 끝난 뒤)
        lv_obj_add_event_cb(ui_RESUMEN_DE_SESSION, on_Resumen_Screen_Loaded, LV_EVENT_SCREEN_LOADED, NULL);
    }

    // 배터리: 거북이가 보내는 값으로 자동 표시 (loop 의 updateBatteryUI).
    // 데이터가 오기 전에는 "--"
    createTortugaBattery();   // 거북이 배터리 (코드로 그림, 배경 이미지의 거북이는 덮음)
    createBallenaBattery();   // 고래 배터리 (거북이 배터리 옆)
    createVolumeControl();    // 볼륨 버튼 + 볼륨 창 (고래 배터리 오른쪽)
    updateBatteryUI();

    createEstopBanner();  // 비상정지 안내창 (평소에는 숨김)
    createConnPanel();    // CONEXION BLUETOOTH 화면의 연결 상태 표시
    createPacienteLabel(); // MODO REHABILITACION: 현재 환자 ID (PAC-101 ...)
    createFlechas();       // ▲▼ 화살표 (발표용 수동 조작, SquareLine 수정 불필요)

    // 게임 모드 (MODO JUEGO) 초기화
    puntos = 0;
    updatePuntosUI();
    generateRandomColor(false); // sin voz al arrancar

    // 기존 재활 모드 업데이트
    updateRehabUI();
    Serial.println("UI Setup Completed via LovyanGFX");
    Serial.println("TRIAL,n,modo,t_s,latencia_ms,duracion_ms,pico_rms_mV,pico_uV_electrodo,umbral_mV,salida,conteo,enlace");
}


void loop() {
    // El tiempo de LVGL sale de millis() (LV_TICK_CUSTOM 1 en lv_conf.h), no de
    // lv_tick_inc(5): asi no se atrasa cuando dibujar tarda mas de 5 ms.
    uint32_t t0 = millis();
    lv_timer_handler(); // LVGL 화면 업데이트 엔진 실행
    uint32_t dt = millis() - t0;
    if ((DEBUG_TIMING && dt > 20) || dt > 300) {   // 300 ms 이상 멈추면 항상 알려줌
        Serial.printf("lv_timer_handler: %lu ms\n", (unsigned long)dt);
    }
    delay(2);
    audioMusic(true);   // 배경 음악 모든 화면에서 재생

    // EMG: 보정 진행, 거북이 제어, 화면 표시, Serial (플롯/명령)
    emgCalibrationUpdate();
    estopUpdate();
    turtleUpdate();
    updateBatteryUI();
    lv_obj_t *scr = lv_scr_act();
    emgPanelUpdate(scr == ui_MODO_JUEGO || scr == ui_MODO_REHABILITACION);
    emgSerialUpdate();
    espejoUpdate();   // espejo de la pantalla en la computadora (solo con tools/espejo.html abierto)
    sessionLogLoop();
    if (USAR_BLUETOOTH_APP) bleAppLoop();
    firestoreResultUpdate();   // aviso "Enviado a la app" / error
    updateGrabarButton();
    updateConnPanel();

    // 연결 완료 = 거북이가 ESP-NOW 메시지를 실제로 받았고(ACK) + EMG 신호가 정상
    isBluetoothConnected = allConnected();

    // 자동 화면 전환을 위한 타이머 변수
    static unsigned long transitionDelayStart = 0;
    static bool readyToTransition = false;

    // 블루투스 연결 화면 로직
    if (scr == ui_CONEXION_BLUETOOTH) {

        if (!isBluetoothConnected) {
            // (1) 연결 중일 때: 기존 로딩 애니메이션 작동
            readyToTransition = false;
            if (millis() - lastMillis > 200) {
                lastMillis = millis();
                btLoadingValue += 10;
                if (btLoadingValue > 100) {
                    btLoadingValue = 0;
                }
            }
            lv_bar_set_value(ui_BluetoothBAR2, btLoadingValue, LV_ANIM_OFF);

        } else {
            // (2) 연결이 완료된 상태일 때: 자연스러운 자동 화면 전환
            // 게이지 바를 100%로 가득 채워 완료된 시각적 효과 제공
            lv_bar_set_value(ui_BluetoothBAR2, 100, LV_ANIM_OFF);

            // 딜레이 시작 시간을 기록
            if (!readyToTransition) {
                transitionDelayStart = millis();
                readyToTransition = true;
            }

            // 2000ms(2초)가 지나면 화면 자동 전환
            if (millis() - transitionDelayStart > 2000) {
                _ui_screen_change(&ui_SELECCION_DE_MODO, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, &ui_SELECCION_DE_MODO_screen_init);

                // 다시 이 화면으로 돌아올 경우를 대비해 초기화
                readyToTransition = false;
            }
        }
    } else {
        // 다른 화면에 있을 때는 타이머 리셋
        readyToTransition = false;
    }
}
