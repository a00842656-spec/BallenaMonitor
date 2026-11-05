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
constexpr uint8_t TURTLE_SPEED = 180;

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

// Actualiza las 6 barras/etiquetas de bateria (una por pantalla)
static void updateBatteryUI() {
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

    lv_obj_t *labels[] = {ui_lblBatteryTxt1, ui_lblBatteryTxt2, ui_lblBatteryTxt3,
                          ui_lblBatteryTxt4, ui_lblBatteryTxt5, ui_lblBatteryTxt6};
    lv_obj_t *bars[] = {ui_BatteryBAR1, ui_BatteryBAR2, ui_BatteryBAR3,
                        ui_BatteryBAR4, ui_BatteryBAR5, ui_BatteryBAR6};
    for (int i = 0; i < 6; i++) {
        if (labels[i] != NULL) lv_label_set_text(labels[i], txt);
        if (bars[i] != NULL) {
            lv_bar_set_value(bars[i], pct < 0 ? 0 : pct, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bars[i], col, LV_PART_INDICATOR);
        }
    }
    if (pct >= 0) {
        Serial.printf("[BATERIA] tortuga %u mV = %d%%\n", (unsigned)turtleBatteryMv, pct);
    }
}

static uint8_t lastSentCmd = MOTOR_CMD_STOP;
static uint32_t lastSendMs = 0;

void sendMotor(uint8_t cmd, uint8_t speed) {
    MotorMsg msg = {MOTOR_MSG_MAGIC, cmd, speed};
    esp_err_t result = esp_now_send(turtleAddress, (uint8_t *)&msg, sizeof(msg));
    if (result != ESP_OK) {
        Serial.printf("Error al enviar comando %u\n", cmd);
    }
    lastSentCmd = cmd;
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
constexpr uint32_t DRAW_BUF_LINES = 40;
static lv_color_t *drawBuf1 = NULL;
static lv_color_t *drawBuf2 = NULL;

// LVGL 디스플레이 플러시 함수
void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

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

    if (!touched) {
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
    } else if (s.active && emgDriveAllowed(scr)) {
        // 수축하는 동안 100 ms 마다 전진 명령 (거북이는 300 ms 동안 못 받으면 스스로 정지)
        if (lastSentCmd != MOTOR_CMD_FORWARD || now - lastSendMs >= MOTOR_RESEND_MS) {
            sendMotor(MOTOR_CMD_FORWARD, TURTLE_SPEED);
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
    Serial.begin(115200);
    audioBegin();

    if (ESTOP_PIN >= 0) pinMode(ESTOP_PIN, INPUT_PULLUP);

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

    // 배터리: 거북이가 보내는 값으로 자동 표시 (loop 의 updateBatteryUI).
    // 데이터가 오기 전에는 "--"
    updateBatteryUI();

    createEstopBanner();  // 비상정지 안내창 (평소에는 숨김)
    createConnPanel();    // CONEXION BLUETOOTH 화면의 연결 상태 표시
    createPacienteLabel(); // MODO REHABILITACION: 현재 환자 ID (PAC-101 ...)

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
    if (DEBUG_TIMING && dt > 20) {
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
