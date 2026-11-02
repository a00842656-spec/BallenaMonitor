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

// MAC del XIAO de la tortuga (la imprime su Serial: "XIAO MAC: ...").
// Con FF:FF:FF:FF:FF:FF (broadcast) tambien funciona, sin conocer la MAC.
uint8_t turtleAddress[] = {0xE4, 0xB3, 0x23, 0xB6, 0x0A, 0x80};

esp_now_peer_info_t peerInfo;

// Velocidad de la tortuga al presionar las flechas (duty PWM 0..255)
constexpr uint8_t TURTLE_SPEED = 180;

// true = imprimir por Serial cuando dibujar la pantalla tarda mas de 20 ms
constexpr bool DEBUG_TIMING = true;

void sendMotor(uint8_t cmd, uint8_t speed) {
  MotorMsg msg = {MOTOR_MSG_MAGIC, cmd, speed};
  esp_err_t result = esp_now_send(turtleAddress, (uint8_t *)&msg, sizeof(msg));
  if (result != ESP_OK) {
    Serial.printf("Error al enviar comando %u\n", cmd);
  }
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
static lv_color_t buf[screenWidth * 40]; 


// LVGL 디스플레이 플러시 함수
void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.pushImage(area->x1, area->y1, w, h, (uint16_t *)&color_p->full);
  tft.endWrite();

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

int puntos = 0;       // 누적 점수 카운터
int lastColor = -1;   // 이전에 나온 색상을 기억하여 중복을 방지하는 변수

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
    lv_obj_move_to_index(ui_Nube4, 1);  // justo encima del fondo, debajo del texto
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

// 5./6. Flechas de la tortuga: el envio se hace en loop() (updateTurtleArrows),
// que repite el comando mientras la flecha siga presionada y manda STOP al soltarla.
// SquareLine solo llama a estas funciones en LV_EVENT_PRESSED, por eso quedan vacias.
void MoveTurtleUP(lv_event_t * e) {}
void MoveTurtleDOWN(lv_event_t * e) {}

// Flechas (MODO JUEGO y MODO REHABILITACION) -> motor de la tortuga por ESP-NOW
void updateTurtleArrows() {
  static uint8_t lastCmd = MOTOR_CMD_STOP;
  static uint32_t lastSendMs = 0;

  uint8_t cmd = MOTOR_CMD_STOP;
  if (isPressed(ui_MoveTurtleUP4) || isPressed(ui_MoveTurtleUP5)) {
    cmd = MOTOR_CMD_FORWARD;
  } else if (isPressed(ui_MoveTurtleDOWN4) || isPressed(ui_MoveTurtleDOWN5)) {
    cmd = MOTOR_CMD_REVERSE;
  }

  // Enviar al cambiar de comando y repetir cada MOTOR_RESEND_MS mientras siga presionada
  // (el XIAO se detiene solo si no recibe nada en MOTOR_FAILSAFE_MS)
  if (cmd != lastCmd || (cmd != MOTOR_CMD_STOP && millis() - lastSendMs >= MOTOR_RESEND_MS)) {
    sendMotor(cmd, cmd == MOTOR_CMD_STOP ? 0 : TURTLE_SPEED);
    if (cmd != lastCmd) {
      Serial.printf("Tortuga: %s\n", cmd == MOTOR_CMD_FORWARD ? "UP" : cmd == MOTOR_CMD_REVERSE ? "DOWN" : "STOP");
    }
    lastCmd = cmd;
    lastSendMs = millis();
  }
}

// 7. 거북이 후진 버튼 이벤트
void Regresar(lv_event_t * e) {
    // 추후 ESP-NOW 통신으로 거북이에게 후진 명령을 보내는 코드 작성 위치 (sendMotor 사용)
    Serial.println("Comando: REGRESAR (Turtle moves back)");
}


// ==========================================
// 재활 모드 (MODO REHABILITACION)

int objetivoVal = 1;    // 시연을 위해 보기 좋게 초기 목표값을 1으로 설정
int progresoVal = 0;    // 환자가 현재까지 달성한 횟수
bool sessionCompleted = false; // 세션 완료 플래그
int ultimoModo = 1;     // 1 = MODO JUEGO, 2 = MODO REHABILITACION

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

// 4. 시연용 센서 작동 버튼 (CONTINUAR 버튼 누를 때마다 횟수 증가)
void ContinueSession(lv_event_t * e) {
    if (progresoVal < objetivoVal) {
        progresoVal++;
        updateRehabUI(); // 화면 갱신
        
        // 목표치에 도달하면 요약 화면으로 자동 전환
        if (progresoVal >= objetivoVal && !sessionCompleted) {
            sessionCompleted = true;
            audioPlaySfx(SFX_CORRECT);
            // TERMINAR 버튼을 누르지 않고 자동 전환될 때도 재활 모드(2) 기억
            ultimoModo = 2;
            _ui_screen_change(&ui_RESUMEN_DE_SESSION, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, &ui_RESUMEN_DE_SESSION_screen_init);
        }
    }
}

// PAUSAR(일시정지) 버튼을 눌렀을 때 작동할 코드
void PauseSession(lv_event_t * e) {
}

// 5. 나중에 실제 하드웨어 센서가 연결되었을 때 호출할 함수
void patient_achieved_rep() {
    ContinueSession(NULL); // 동일한 로직 사용
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
    updateRehabUI();         // 화면 갱신
}
// ==========================================


void setup() {
  Serial.begin(115200);
  audioBegin();

  // --- ESP-NOW 설정 ---
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE); // mismo canal que el XIAO
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  
  memcpy(peerInfo.peer_addr, turtleAddress, 6);
  peerInfo.channel = ESPNOW_CHANNEL;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("Failed to add peer");
    return;
  }
  
  // 통신 준비 완료 플래그 활성화
  isBluetoothConnected = true; 
  // --- ESP-NOW 설정 끝 ---


  // 백라이트 핀 켜기 (GPIO 45)
  pinMode(45, OUTPUT);
  digitalWrite(45, HIGH);

  // LovyanGFX 및 LVGL 초기화
  tft.begin();
  tft.setSwapBytes(true); 
  tft.setRotation(1);  // 가로 모드 (480x320)   
  
  lv_init();

  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 40);

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
  ui_init(); //[cite: 42]
  addBubbleToButtons();

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

  // 배터리 텍스트와 바 게이지 값 변경하기
  lv_label_set_text(ui_lblBatteryTxt1, "15%"); 
  lv_label_set_text(ui_lblBatteryTxt2, "100%"); 
  lv_label_set_text(ui_lblBatteryTxt3, "85%"); 
  lv_label_set_text(ui_lblBatteryTxt4, "85%"); 
  lv_label_set_text(ui_lblBatteryTxt5, "85%");
  lv_label_set_text(ui_lblBatteryTxt6, "85%"); 

  lv_bar_set_value(ui_BatteryBAR1, 15, LV_ANIM_OFF); 
  lv_bar_set_value(ui_BatteryBAR2, 100, LV_ANIM_OFF); 
  lv_bar_set_value(ui_BatteryBAR3, 85, LV_ANIM_OFF); 
  lv_bar_set_value(ui_BatteryBAR4, 85, LV_ANIM_OFF); 
  lv_bar_set_value(ui_BatteryBAR5, 85, LV_ANIM_OFF); 
  lv_bar_set_value(ui_BatteryBAR6, 85, LV_ANIM_OFF); 

  // 게임 모드 (MODO JUEGO) 초기화
  puntos = 0;
  updatePuntosUI();
  generateRandomColor(false); // sin voz al arrancar

  // 기존 재활 모드 업데이트
  updateRehabUI(); 

  Serial.println("UI Setup Completed via LovyanGFX");
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

  updateTurtleArrows(); // flechas -> motor de la tortuga (ESP-NOW)

  audioMusic(true);  // 배경 음악 모든 화면에서 재생

  // 자동 화면 전환을 위한 타이머 변수
  static unsigned long transitionDelayStart = 0;
  static bool readyToTransition = false;

  // 블루투스 연결 화면 로직
  if (lv_scr_act() == ui_CONEXION_BLUETOOTH) {
    
    if (!isBluetoothConnected) {
      // (1) 연결 중일 때: 기존 로딩 애니메이션 작동
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