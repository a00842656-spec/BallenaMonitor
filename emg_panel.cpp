// Ver emg_panel.h
#include "emg_panel.h"

#include <Arduino.h>
#include <Preferences.h>

#include "audio.h"
#include "emg.h"

// ---- Aspecto / posicion por defecto ---------------------------------------------
// Posicion inicial (esquina superior izquierda del panel). Si el panel tapa algo,
// arrastralo con el dedo: la nueva posicion se guarda.
constexpr lv_coord_t PANEL_DEFAULT_X = 90;
constexpr lv_coord_t PANEL_DEFAULT_Y = 40;
constexpr lv_coord_t PANEL_W = 300;
constexpr lv_coord_t PANEL_H = 54;
constexpr lv_coord_t CHART_H = 100;
constexpr uint16_t CHART_POINTS = 100;   // 100 puntos x 50 ms = 5 s
constexpr float BAR_FULL_X_THRESHOLD = 2.5f;  // la barra llena = 2.5 x umbral (umbral al 40 %)

constexpr uint32_t COLOR_IDLE = 0x4FA3D9;
constexpr uint32_t COLOR_ACTIVE = 0x3CCB5A;
constexpr uint32_t COLOR_SAT = 0xE8231E;

static lv_obj_t *panel, *bar, *marker, *label, *chart;
static lv_chart_series_t *serRms, *serThr;
static bool chartShown = false;
static bool dragged = false;
static Preferences uiPrefs;

static void savePos() {
  uiPrefs.putShort("x", lv_obj_get_x(panel));
  uiPrefs.putShort("y", lv_obj_get_y(panel));
}

static void panelEvent(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    dragged = false;
  } else if (code == LV_EVENT_PRESSING) {
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL) return;
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (v.x == 0 && v.y == 0) return;
    lv_coord_t x = lv_obj_get_x(panel) + v.x;
    lv_coord_t y = lv_obj_get_y(panel) + v.y;
    lv_coord_t maxX = lv_disp_get_hor_res(NULL) - lv_obj_get_width(panel);
    lv_coord_t maxY = lv_disp_get_ver_res(NULL) - lv_obj_get_height(panel);
    x = LV_CLAMP(0, x, maxX);
    y = LV_CLAMP(0, y, maxY);
    lv_obj_set_pos(panel, x, y);
    dragged = true;
  } else if (code == LV_EVENT_RELEASED) {
    if (dragged) {
      savePos();
    } else {
      chartShown = !chartShown;
      if (chartShown) {
        lv_obj_clear_flag(chart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(panel, PANEL_H + CHART_H + 6);
      } else {
        lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(panel, PANEL_H);
      }
      // que no se salga de la pantalla al crecer
      lv_coord_t maxY = lv_disp_get_ver_res(NULL) - lv_obj_get_height(panel);
      if (lv_obj_get_y(panel) > maxY) lv_obj_set_y(panel, maxY);
    }
  }
}

static void minusEvent(lv_event_t *e) {
  audioPlaySfx(SFX_BUBBLE);
  emgSetThreshold(emgThreshold() * 0.9f);
}

static void plusEvent(lv_event_t *e) {
  audioPlaySfx(SFX_BUBBLE);
  emgSetThreshold(emgThreshold() * 1.1f);
}

static void calEvent(lv_event_t *e) {
  audioPlaySfx(SFX_BUBBLE);
  if (emgCalibrationState() == CAL_REST || emgCalibrationState() == CAL_SQUEEZE) {
    emgCancelCalibration();
  } else {
    emgStartCalibration();
  }
}

static lv_obj_t *smallButton(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t w, lv_event_cb_t cb) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, w, 38);
  lv_obj_set_pos(b, x, 2);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x1E5B8C), 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_obj_center(l);
  return b;
}

void emgPanelCreate() {
  uiPrefs.begin("emgui", false);

  panel = lv_obj_create(lv_layer_top());
  lv_obj_set_size(panel, PANEL_W, PANEL_H);
  lv_obj_set_style_pad_all(panel, 4, 0);
  lv_obj_set_style_radius(panel, 12, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x06243F), 0);
  lv_obj_set_style_bg_opa(panel, 215, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x6EC6FF), 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(panel, panelEvent, LV_EVENT_ALL, NULL);

  lv_coord_t x = uiPrefs.getShort("x", PANEL_DEFAULT_X);
  lv_coord_t y = uiPrefs.getShort("y", PANEL_DEFAULT_Y);
  if (x < 0 || x > 480 - PANEL_W || y < 0 || y > 320 - PANEL_H) {
    x = PANEL_DEFAULT_X;
    y = PANEL_DEFAULT_Y;
  }
  lv_obj_set_pos(panel, x, y);

  // [-] barra [+] [CAL]   (coordenadas dentro del panel, ancho util 290)
  smallButton(panel, LV_SYMBOL_MINUS, 0, 38, minusEvent);

  bar = lv_bar_create(panel);
  lv_obj_set_size(bar, 148, 14);
  lv_obj_set_pos(bar, 44, 2);
  lv_bar_set_range(bar, 0, 1000);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x0B3A5E), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_IDLE), LV_PART_INDICATOR);
  lv_obj_set_style_anim_time(bar, 0, 0);

  // Marca del umbral: 40 % de la barra
  marker = lv_obj_create(panel);
  lv_obj_set_size(marker, 3, 20);
  lv_obj_set_pos(marker, 44 + (lv_coord_t)(148 / BAR_FULL_X_THRESHOLD) - 1, -1);
  lv_obj_set_style_bg_color(marker, lv_color_white(), 0);
  lv_obj_set_style_border_width(marker, 0, 0);
  lv_obj_set_style_radius(marker, 1, 0);
  lv_obj_clear_flag(marker, LV_OBJ_FLAG_CLICKABLE);

  label = lv_label_create(panel);
  lv_obj_set_pos(label, 44, 20);
  lv_obj_set_width(label, 152);
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
  lv_label_set_text(label, "EMG");

  smallButton(panel, LV_SYMBOL_PLUS, 198, 38, plusEvent);
  smallButton(panel, "CAL", 240, 50, calEvent);

  chart = lv_chart_create(panel);
  lv_obj_set_size(chart, PANEL_W - 12, CHART_H);
  lv_obj_set_pos(chart, 0, PANEL_H - 6);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart, CHART_POINTS);
  lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_div_line_count(chart, 3, 0);
  lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);  // sin puntos, solo lineas
  lv_obj_set_style_bg_color(chart, lv_color_hex(0x031828), 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_pad_all(chart, 2, 0);
  lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
  serRms = lv_chart_add_series(chart, lv_color_hex(COLOR_IDLE), LV_CHART_AXIS_PRIMARY_Y);
  serThr = lv_chart_add_series(chart, lv_color_white(), LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_all_value(chart, serRms, 0);
  lv_chart_set_all_value(chart, serThr, 0);
  lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

void emgPanelUpdate(bool visible) {
  if (panel == NULL) return;

  if (!visible) {
    if (!lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN)) lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);

  static uint32_t lastFast = 0, lastText = 0;
  uint32_t now = millis();
  if (now - lastFast < 50) return;  // 20 Hz
  lastFast = now;

  EmgSnapshot s = emgRead();
  float thr = emgThreshold();
  float full = thr * BAR_FULL_X_THRESHOLD;

  int v = (int)(s.rmsMv / full * 1000.0f);
  lv_bar_set_value(bar, LV_CLAMP(0, v, 1000), LV_ANIM_OFF);
  uint32_t col = s.saturated ? COLOR_SAT : s.active ? COLOR_ACTIVE : COLOR_IDLE;
  lv_obj_set_style_bg_color(bar, lv_color_hex(col), LV_PART_INDICATOR);

  if (chartShown) {
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, (lv_coord_t)LV_MAX(10.0f, full));
    lv_chart_set_series_color(chart, serRms, lv_color_hex(col));
    lv_chart_set_next_value(chart, serRms, (lv_coord_t)LV_MIN(s.rmsMv, full));
    lv_chart_set_next_value(chart, serThr, (lv_coord_t)thr);
  }

  if (now - lastText < 200) return;  // texto a 5 Hz
  lastText = now;

  char buf[48];
  EmgCalState cs = emgCalibrationState();
  uint32_t rem = (emgCalibrationRemainingMs() + 999) / 1000;
  switch (cs) {
    case CAL_REST:
      snprintf(buf, sizeof(buf), "Relaja la mano... %lu", (unsigned long)rem);
      break;
    case CAL_SQUEEZE:
      snprintf(buf, sizeof(buf), "Aprieta fuerte! %lu", (unsigned long)rem);
      break;
    case CAL_DONE:
      snprintf(buf, sizeof(buf), "Listo: umbral %.0f", (double)thr);
      break;
    case CAL_FAILED:
      snprintf(buf, sizeof(buf), "Fallo: revisar electrodos");
      break;
    default:
      if (s.saturated) {
        snprintf(buf, sizeof(buf), "SATURADA %.0f/%.0f", (double)s.rmsMv, (double)thr);
      } else {
        snprintf(buf, sizeof(buf), "%.0f / %.0f mV", (double)s.rmsMv, (double)thr);
      }
      break;
  }
  lv_label_set_text(label, buf);
}
