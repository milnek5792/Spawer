// ui_test.cpp — Spawer op-test: D0–D9 + C0–C11 + SP H101 int°C + H100 Start + H13
#include "ui_test.h"

#include "board_7b.h"
#include "gt911.h"
#include "i2c.h"
#include "io_extension.h"
#include "rgb_lcd_port.h"
#include "touch.h"

#include <Arduino.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace {

esp_lcd_panel_handle_t s_panel = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
lv_display_t *s_disp = nullptr;
lv_indev_t *s_indev = nullptr;
void *s_fb0 = nullptr;
void *s_fb1 = nullptr;
size_t s_fbBytes = 0;
SemaphoreHandle_t s_vsyncSem = nullptr;
uint32_t s_lastTickMs = 0;
bool s_ok = false;
bool s_touchArmed = false;
int16_t s_touchLastX = 0;
int16_t s_touchLastY = 0;

// Manual D0–D9 short ASCII names (Montserrat has no Czech diacritics).
static const char *const kDNames[UI_D_COUNT] = {
    "Okolni",      // D0
    "Vstup",       // D1
    "Vystup",      // D2
    "Civka",       // D3
    "Kompresor",   // D4
    "Ventilator",  // D5
    "4cestny",     // D6
    "HiTlak",      // D7
    "LoTlak",      // D8
    "Prutok",      // D9
};

// D0–D3 = measured temps (°C ×0.1, live raw 200≈20.0); D4–D9 = status.
static bool isTempD(int i) { return i >= 0 && i <= 3; }

// Manual C0–C11 short names; values are integer °C or 0/1 (NOT ×0.1).
static const char *const kCNames[UI_C_COUNT] = {
    "SP vytop",    // C0 setpoint heating → H101
    "dT restart",  // C1
    "AutoRst",     // C2 0/1
    "T discharge", // C3
    "Tmax heat",   // C4
    "Tmin heat",   // C5
    "dT stop H",   // C6
    "SP chlaz",    // C7 cooling setpoint
    "dT rst cool", // C8
    "dT stop C",   // C9
    "Tmax cool",   // C10
    "Tmin cool",   // C11
};
// C2 is 0/1 flag; others shown as integer (mostly °C).
static bool isFlagC(int i) { return i == 2; }

lv_obj_t *s_lblTitle = nullptr;
lv_obj_t *s_lblD[UI_D_COUNT] = {};
lv_obj_t *s_lblC[UI_C_COUNT] = {};
lv_obj_t *s_lblSp = nullptr;
lv_obj_t *s_lblH100 = nullptr;
lv_obj_t *s_lblH13 = nullptr;
lv_obj_t *s_lblStatus = nullptr;
lv_obj_t *s_btnStart = nullptr;
lv_obj_t *s_btnStop = nullptr;
lv_obj_t *s_btnSpDown = nullptr;
lv_obj_t *s_btnSpUp = nullptr;

volatile bool s_startReq = false;
volatile bool s_stopReq = false;
volatile bool s_spDownReq = false;
volatile bool s_spUpReq = false;

bool s_writeBusy = false;
uint32_t s_lastTapMs = 0;
static const uint32_t TAP_DEBOUNCE_MS = 450;

UiTestSnap s_last = {};

char s_txtD[UI_D_COUNT][48] = {};
char s_txtC[UI_C_COUNT][40] = {};
char s_txtSp[64] = "";
char s_txtH100[32] = "";
char s_txtH13[72] = "";
char s_txtStatus[96] = "";
int s_btnColorState = -1;  // -1 unknown, 0 stopped(green START), 1 running(amber)

bool onVsync(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata,
             void *user_ctx) {
  (void)panel;
  (void)edata;
  (void)user_ctx;
  BaseType_t hp = pdFALSE;
  if (s_vsyncSem) {
    xSemaphoreGiveFromISR(s_vsyncSem, &hp);
  }
  return hp == pdTRUE;
}

void dispFlush(lv_display_t *disp, const lv_area_t *area, uint8_t *pxMap) {
  (void)area;
  esp_lcd_panel_handle_t panel =
      static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(disp));
  if (panel && lv_display_flush_is_last(disp) && pxMap) {
    esp_lcd_panel_draw_bitmap(panel, 0, 0, BOARD_PANEL_W, BOARD_PANEL_H, pxMap);
    if (s_vsyncSem) {
      xSemaphoreTake(s_vsyncSem, 0);
      xSemaphoreTake(s_vsyncSem, pdMS_TO_TICKS(100));
    }
  }
  lv_display_flush_ready(disp);
}

void touchDrain() {
  if (!s_touch) return;
  for (int i = 0; i < 8; ++i) {
    esp_lcd_touch_read_data(s_touch);
    uint16_t x[1], y[1], s[1];
    uint8_t n = 0;
    esp_lcd_touch_get_coordinates(s_touch, x, y, s, &n, 1);
  }
}

void touchRead(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  if (!s_touch || !s_touchArmed) {
    data->state = LV_INDEV_STATE_RELEASED;
    data->point.x = s_touchLastX;
    data->point.y = s_touchLastY;
    return;
  }

  esp_lcd_touch_read_data(s_touch);
  uint16_t x[1] = {0};
  uint16_t y[1] = {0};
  uint16_t strength[1] = {0};
  uint8_t count = 0;
  const bool pressed =
      esp_lcd_touch_get_coordinates(s_touch, x, y, strength, &count, 1) && count > 0;

  if (pressed) {
    int16_t px = (int16_t)x[0];
    int16_t py = (int16_t)y[0];
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= BOARD_PANEL_W) px = BOARD_PANEL_W - 1;
    if (py >= BOARD_PANEL_H) py = BOARD_PANEL_H - 1;
    s_touchLastX = px;
    s_touchLastY = py;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  data->point.x = s_touchLastX;
  data->point.y = s_touchLastY;
}

bool acceptTap(void) {
  if (s_writeBusy) return false;
  uint32_t now = millis();
  if (s_lastTapMs != 0 && (now - s_lastTapMs) < TAP_DEBOUNCE_MS) return false;
  s_lastTapMs = now;
  return true;
}

void onStartClicked(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (!acceptTap()) {
    Serial.println(F("H100_START tap IGNORED (busy/debounce)"));
    return;
  }
  s_startReq = true;
  Serial.println(F("H100_START tap (UI CLICKED)"));
}

void onStopClicked(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (!acceptTap()) {
    Serial.println(F("H100_STOP tap IGNORED (busy/debounce)"));
    return;
  }
  s_stopReq = true;
  Serial.println(F("H100_STOP tap (UI CLICKED)"));
}

void onSpDownClicked(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (!acceptTap()) {
    Serial.println(F("SP- tap IGNORED (busy/debounce)"));
    return;
  }
  s_spDownReq = true;
  Serial.println(F("SP- tap (UI CLICKED −1 C, C0 int)"));
}

void onSpUpClicked(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (!acceptTap()) {
    Serial.println(F("SP+ tap IGNORED (busy/debounce)"));
    return;
  }
  s_spUpReq = true;
  Serial.println(F("SP+ tap (UI CLICKED +1 C, C0 int)"));
}

void setLabelIfChanged(lv_obj_t *lbl, char *cache, size_t cacheCap, const char *text) {
  if (!lbl || !text || !cache) return;
  if (strncmp(cache, text, cacheCap) == 0) return;
  strncpy(cache, text, cacheCap - 1);
  cache[cacheCap - 1] = '\0';
  lv_label_set_text(lbl, cache);
}

/** D measured temps: raw×0.1 °C (live H1=200 → 20.0). */
void fmtTempC_x01(char *buf, size_t cap, uint16_t raw) {
  int whole = (int)raw / 10;
  int frac = (int)raw % 10;
  snprintf(buf, cap, "%d.%d", whole, frac);
}

/** Manual C / SP: integer °C (raw 2 → "2", never "0.2"). */
void fmtIntC(char *buf, size_t cap, uint16_t raw) {
  snprintf(buf, cap, "%u", (unsigned)raw);
}

void fmtDValue(char *buf, size_t cap, int idx, uint16_t raw, bool have) {
  if (!have) {
    if (isTempD(idx)) {
      snprintf(buf, cap, "D%d %-10s  —.- C", idx, kDNames[idx]);
    } else {
      snprintf(buf, cap, "D%d %-10s  —", idx, kDNames[idx]);
    }
    return;
  }
  if (isTempD(idx)) {
    char t[16];
    fmtTempC_x01(t, sizeof(t), raw);
    snprintf(buf, cap, "D%d %-10s  %s C", idx, kDNames[idx], t);
  } else if (raw <= 1) {
    snprintf(buf, cap, "D%d %-10s  %s", idx, kDNames[idx], raw ? "ZAP" : "VYP");
  } else {
    snprintf(buf, cap, "D%d %-10s  %u", idx, kDNames[idx], (unsigned)raw);
  }
}

void fmtCValue(char *buf, size_t cap, int idx, uint16_t raw, bool have) {
  if (idx < 0 || idx >= UI_C_COUNT) {
    snprintf(buf, cap, "C? —");
    return;
  }
  if (!have) {
    if (isFlagC(idx)) {
      snprintf(buf, cap, "C%d %-10s —", idx, kCNames[idx]);
    } else {
      snprintf(buf, cap, "C%d %-10s — C", idx, kCNames[idx]);
    }
    return;
  }
  if (isFlagC(idx)) {
    snprintf(buf, cap, "C%d %-10s %s", idx, kCNames[idx], raw ? "1" : "0");
  } else {
    snprintf(buf, cap, "C%d %-10s %u C", idx, kCNames[idx], (unsigned)raw);
  }
}

lv_obj_t *makeSpButton(lv_obj_t *scr, const char *txt, lv_align_t align, int32_t x,
                       int32_t y, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_button_create(scr);
  lv_obj_set_size(btn, 88, 56);
  lv_obj_align(btn, align, x, y);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A4A5C), 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A3542), LV_STATE_PRESSED);
  lv_obj_set_style_radius(btn, 10, 0);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, txt);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(lbl);
  lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
  return btn;
}

static const uint32_t kDColors[UI_D_COUNT] = {
    0xA5B4C8,  // D0
    0x7EB6FF,  // D1
    0x7FDBCA,  // D2
    0xC5B4E3,  // D3
    0xF4A261,  // D4
    0xE9C46A,  // D5
    0x90BE6D,  // D6
    0xFF6B6B,  // D7
    0xFF8E72,  // D8
    0x4ECDC4,  // D9
};

void buildScreen() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1116), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  s_lblTitle = lv_label_create(scr);
  lv_label_set_text(s_lblTitle,
                    "Spawer — D0..D9 + C0..C11 (C0=SP H101 int) + H100 Start");
  lv_obj_set_style_text_color(s_lblTitle, lv_color_hex(0xE8EEF7), 0);
  lv_obj_set_style_text_font(s_lblTitle, &lv_font_montserrat_16, 0);
  lv_obj_align(s_lblTitle, LV_ALIGN_TOP_MID, 0, 4);

  // Compact 2×5 D0..D9 (measured; temps ×0.1).
  const int32_t gridTop = 26;
  const int32_t rowH = 22;
  const int32_t col0X = 28;
  const int32_t col1X = 520;
  for (int i = 0; i < UI_D_COUNT; i++) {
    int row = i / 2;
    int col = i % 2;
    int32_t x = (col == 0) ? col0X : col1X;
    int32_t y = gridTop + row * rowH;

    s_lblD[i] = lv_label_create(scr);
    char init[48];
    fmtDValue(init, sizeof(init), i, 0, false);
    lv_label_set_text(s_lblD[i], init);
    strncpy(s_txtD[i], init, sizeof(s_txtD[i]) - 1);
    lv_obj_set_style_text_color(s_lblD[i], lv_color_hex(kDColors[i]), 0);
    lv_obj_set_style_text_font(s_lblD[i], &lv_font_montserrat_14, 0);
    lv_obj_align(s_lblD[i], LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_width(s_lblD[i], 460);
  }

  // Compact 3×4 C0..C11 (H101..H112); integer °C / flags — NOT ×0.1.
  const int32_t cTop = gridTop + 5 * rowH + 4;
  const int32_t cRowH = 20;
  const int32_t cColW = 330;
  for (int i = 0; i < UI_C_COUNT; i++) {
    int row = i / 3;
    int col = i % 3;
    int32_t x = 28 + col * cColW;
    int32_t y = cTop + row * cRowH;
    s_lblC[i] = lv_label_create(scr);
    char init[40];
    fmtCValue(init, sizeof(init), i, 0, false);
    lv_label_set_text(s_lblC[i], init);
    strncpy(s_txtC[i], init, sizeof(s_txtC[i]) - 1);
    lv_obj_set_style_text_color(s_lblC[i], lv_color_hex(0x9EC5E8), 0);
    lv_obj_set_style_text_font(s_lblC[i], &lv_font_montserrat_14, 0);
    lv_obj_align(s_lblC[i], LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_width(s_lblC[i], cColW - 8);
  }

  // SP = C0 = holding 101 — integer °C (manual C0 15–40), Start stays H100.
  const int32_t spY = cTop + 4 * cRowH + 6;
  s_lblSp = lv_label_create(scr);
  lv_label_set_text(s_lblSp, "C0/SP H101  — C");
  strncpy(s_txtSp, "C0/SP H101  — C", sizeof(s_txtSp) - 1);
  lv_obj_set_style_text_color(s_lblSp, lv_color_hex(0xF4A261), 0);
  lv_obj_set_style_text_font(s_lblSp, &lv_font_montserrat_22, 0);
  lv_obj_align(s_lblSp, LV_ALIGN_TOP_LEFT, 28, spY);

  s_btnSpDown = makeSpButton(scr, "-", LV_ALIGN_TOP_LEFT, 360, spY - 8, onSpDownClicked);
  s_btnSpUp = makeSpButton(scr, "+", LV_ALIGN_TOP_LEFT, 460, spY - 8, onSpUpClicked);

  s_lblH100 = lv_label_create(scr);
  lv_label_set_text(s_lblH100, "H100 Start/Stop  —");
  strncpy(s_txtH100, "H100 Start/Stop  —", sizeof(s_txtH100) - 1);
  lv_obj_set_style_text_color(s_lblH100, lv_color_hex(0xFFD166), 0);
  lv_obj_set_style_text_font(s_lblH100, &lv_font_montserrat_18, 0);
  lv_obj_align(s_lblH100, LV_ALIGN_TOP_LEFT, 28, spY + 52);

  s_lblH13 = lv_label_create(scr);
  lv_label_set_text(s_lblH13, "H13  —");
  strncpy(s_txtH13, "H13  —", sizeof(s_txtH13) - 1);
  lv_obj_set_style_text_color(s_lblH13, lv_color_hex(0xFF6B6B), 0);
  lv_obj_set_style_text_font(s_lblH13, &lv_font_montserrat_14, 0);
  lv_obj_align(s_lblH13, LV_ALIGN_TOP_LEFT, 420, spY + 54);
  lv_obj_set_width(s_lblH13, BOARD_PANEL_W - 450);

  s_btnStart = lv_button_create(scr);
  lv_obj_set_size(s_btnStart, 420, 120);
  lv_obj_align(s_btnStart, LV_ALIGN_BOTTOM_LEFT, 48, -40);
  lv_obj_set_style_bg_color(s_btnStart, lv_color_hex(0x1F8A4C), 0);
  lv_obj_set_style_bg_color(s_btnStart, lv_color_hex(0x146338), LV_STATE_PRESSED);
  lv_obj_set_style_radius(s_btnStart, 16, 0);
  lv_obj_add_flag(s_btnStart, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_btnStart, onStartClicked, LV_EVENT_CLICKED, nullptr);
  s_btnColorState = 0;

  lv_obj_t *btnStartLbl = lv_label_create(s_btnStart);
  lv_label_set_text(btnStartLbl, "START");
  lv_obj_set_style_text_font(btnStartLbl, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(btnStartLbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(btnStartLbl);
  lv_obj_clear_flag(btnStartLbl, LV_OBJ_FLAG_CLICKABLE);

  s_btnStop = lv_button_create(scr);
  lv_obj_set_size(s_btnStop, 420, 120);
  lv_obj_align(s_btnStop, LV_ALIGN_BOTTOM_RIGHT, -48, -40);
  lv_obj_set_style_bg_color(s_btnStop, lv_color_hex(0xB33A3A), 0);
  lv_obj_set_style_bg_color(s_btnStop, lv_color_hex(0x7A2424), LV_STATE_PRESSED);
  lv_obj_set_style_radius(s_btnStop, 16, 0);
  lv_obj_add_flag(s_btnStop, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_btnStop, onStopClicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *btnStopLbl = lv_label_create(s_btnStop);
  lv_label_set_text(btnStopLbl, "STOP");
  lv_obj_set_style_text_font(btnStopLbl, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(btnStopLbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(btnStopLbl);
  lv_obj_clear_flag(btnStopLbl, LV_OBJ_FLAG_CLICKABLE);

  s_lblStatus = lv_label_create(scr);
  const char *st0 =
      "D=H0..9  |  C0..C11=H101..112 int C  |  SP+/-1=FC16 H101  |  START/STOP=FC16 H100";
  lv_label_set_text(s_lblStatus, st0);
  strncpy(s_txtStatus, st0, sizeof(s_txtStatus) - 1);
  lv_obj_set_style_text_color(s_lblStatus, lv_color_hex(0x8A93A3), 0);
  lv_obj_set_style_text_font(s_lblStatus, &lv_font_montserrat_14, 0);
  lv_obj_align(s_lblStatus, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_set_width(s_lblStatus, BOARD_PANEL_W - 80);
  lv_label_set_long_mode(s_lblStatus, LV_LABEL_LONG_WRAP);
}

}  // namespace

bool uiTestInit(void) {
  Serial.println(F("[UI] Waveshare 7B RGB + GT911 + LVGL bring-up…"));

  DEV_I2C_Init();
  IO_EXTENSION_Init();
  IO_EXTENSION_Output(IO_EXTENSION_IO_5, 0);
  IO_EXTENSION_Output(IO_EXTENSION_IO_3, 0);
  delay(20);
  IO_EXTENSION_Output(IO_EXTENSION_IO_3, 1);
  delay(20);

  s_panel = waveshare_esp32_s3_rgb_lcd_init();
  if (!s_panel) {
    Serial.println(F("[UI] RGB LCD init FAILED"));
    return false;
  }

  waveshare_rgb_lcd_set_brightness(70);
  waveshare_get_frame_buffer(&s_fb0, &s_fb1);
  if (!s_fb0 || !s_fb1) {
    Serial.println(F("[UI] framebuffers missing"));
    return false;
  }
  s_fbBytes = (size_t)BOARD_PANEL_W * (size_t)BOARD_PANEL_H * sizeof(uint16_t);
  memset(s_fb0, 0x10, s_fbBytes);
  memset(s_fb1, 0x10, s_fbBytes);

  s_vsyncSem = xSemaphoreCreateBinary();
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = onVsync;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, nullptr));

  s_touch = touch_gt911_init();
  if (!s_touch) {
    Serial.println(F("[UI] GT911 init FAILED (UI continues without touch)"));
  } else {
    touchDrain();
  }

  lv_init();

  s_disp = lv_display_create(BOARD_PANEL_W, BOARD_PANEL_H);
  lv_display_set_default(s_disp);
  lv_display_set_user_data(s_disp, s_panel);
  lv_display_set_flush_cb(s_disp, dispFlush);
  lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(s_disp, s_fb0, s_fb1, (uint32_t)s_fbBytes,
                         LV_DISPLAY_RENDER_MODE_FULL);

  s_indev = lv_indev_create();
  lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(s_indev, s_disp);
  lv_indev_set_read_cb(s_indev, touchRead);
  lv_indev_set_scroll_limit(s_indev, 25);
  lv_indev_enable(s_indev, true);

  buildScreen();

  s_lastTickMs = millis();
  lv_obj_invalidate(lv_screen_active());
  const uint32_t warmStart = millis();
  while (millis() - warmStart < 400) {
    lv_tick_inc(5);
    lv_timer_handler();
    delay(5);
  }
  touchDrain();
  if (s_indev) {
    lv_indev_reset(s_indev, nullptr);
  }
  s_touchArmed = true;

  s_ok = true;
  Serial.printf("[UI] OK %dx%d touch=%s (D + C0-C11 + SP H101 int + H100 Start)\n",
                BOARD_PANEL_W, BOARD_PANEL_H, s_touch ? "GT911" : "none");
  return true;
}

void uiTestTick(void) {
  if (!s_ok) return;
  uint32_t now = millis();
  if (now != s_lastTickMs) {
    uint32_t dt = now - s_lastTickMs;
    lv_tick_inc(dt > 30 ? 30 : dt);
    s_lastTickMs = now;
  }
  lv_timer_handler();
}

void uiTestUpdate(const UiTestSnap *snap) {
  if (!s_ok || !snap) return;
  s_last = *snap;
  s_last.status = nullptr;

  char buf[64];
  char t[16];

  for (int i = 0; i < UI_D_COUNT; i++) {
    fmtDValue(buf, sizeof(buf), i, snap->dRaw[i], snap->haveD[i]);
    setLabelIfChanged(s_lblD[i], s_txtD[i], sizeof(s_txtD[i]), buf);
  }

  for (int i = 0; i < UI_C_COUNT; i++) {
    fmtCValue(buf, sizeof(buf), i, snap->cRaw[i], snap->haveC[i]);
    setLabelIfChanged(s_lblC[i], s_txtC[i], sizeof(s_txtC[i]), buf);
  }

  // C0/SP: integer °C (manual C) — raw 2 → "2 C", never "0.2".
  if (snap->haveSp) {
    fmtIntC(t, sizeof(t), snap->spRaw);
    snprintf(buf, sizeof(buf), "C0/SP H%u  %s C", (unsigned)snap->spAddr, t);
  } else {
    snprintf(buf, sizeof(buf), "C0/SP H%u  — C",
             (unsigned)(snap->spAddr ? snap->spAddr : 101));
  }
  setLabelIfChanged(s_lblSp, s_txtSp, sizeof(s_txtSp), buf);

  if (snap->haveH100) {
    snprintf(buf, sizeof(buf), "H100 Start/Stop  %u", (unsigned)snap->h100);
  } else {
    snprintf(buf, sizeof(buf), "H100 Start/Stop  —");
  }
  setLabelIfChanged(s_lblH100, s_txtH100, sizeof(s_txtH100), buf);

  if (snap->haveH13) {
    char bits[40];
    size_t n = 0;
    bits[0] = '\0';
    bool first = true;
    for (uint8_t b = 0; b < 16; b++) {
      if ((snap->h13 & (uint16_t)(1u << b)) == 0) continue;
      if (!first && n + 1 < sizeof(bits)) {
        bits[n++] = ',';
        bits[n] = '\0';
      }
      first = false;
      int w = snprintf(bits + n, sizeof(bits) - n, "%u", (unsigned)b);
      if (w < 0) break;
      n += (size_t)w;
      if (n >= sizeof(bits)) break;
    }
    if (first) {
      snprintf(buf, sizeof(buf), "H13  %u  0x%X  bits=-", (unsigned)snap->h13,
               (unsigned)snap->h13);
    } else {
      snprintf(buf, sizeof(buf), "H13  %u  0x%X  bits=%s", (unsigned)snap->h13,
               (unsigned)snap->h13, bits);
    }
  } else {
    snprintf(buf, sizeof(buf), "H13  —");
  }
  setLabelIfChanged(s_lblH13, s_txtH13, sizeof(s_txtH13), buf);

  if (snap->status && *snap->status) {
    setLabelIfChanged(s_lblStatus, s_txtStatus, sizeof(s_txtStatus), snap->status);
  }

  if (s_btnStart && snap->haveH100) {
    int want = snap->h100 ? 1 : 0;
    if (want != s_btnColorState) {
      s_btnColorState = want;
      lv_obj_set_style_bg_color(
          s_btnStart,
          want ? lv_color_hex(0xC47A16) : lv_color_hex(0x1F8A4C), 0);
    }
  }
}

void uiTestUpdateBasic(uint16_t h1, bool haveH1, uint16_t h100, bool haveH100,
                       uint16_t h101, bool haveH101, uint16_t h13, bool haveH13,
                       const char *status) {
  UiTestSnap s = s_last;
  // Compat: h1 historically meant water temp — map to D1 vstup (holding 1).
  s.dRaw[1] = h1;
  s.haveD[1] = haveH1;
  s.h100 = h100;
  s.haveH100 = haveH100;
  s.h101 = h101;
  s.haveH101 = haveH101;
  if (haveH101) {
    s.spRaw = h101;
    s.haveSp = true;
    if (s.spAddr == 0) s.spAddr = 101;
  }
  s.h13 = h13;
  s.haveH13 = haveH13;
  s.status = status;
  uiTestUpdate(&s);
}

bool uiTestConsumeStartRequest(void) {
  if (!s_startReq) return false;
  s_startReq = false;
  if (s_writeBusy) return false;
  return true;
}

bool uiTestConsumeStopRequest(void) {
  if (!s_stopReq) return false;
  s_stopReq = false;
  if (s_writeBusy) return false;
  return true;
}

bool uiTestConsumeSpDownRequest(void) {
  if (!s_spDownReq) return false;
  s_spDownReq = false;
  if (s_writeBusy) return false;
  return true;
}

bool uiTestConsumeSpUpRequest(void) {
  if (!s_spUpReq) return false;
  s_spUpReq = false;
  if (s_writeBusy) return false;
  return true;
}

void uiTestSetWriteBusy(bool busy) {
  s_writeBusy = busy;
  if (busy) {
    s_startReq = false;
    s_stopReq = false;
    s_spDownReq = false;
    s_spUpReq = false;
  }
}

void uiTestSetStatus(const char *status) {
  if (!s_ok || !s_lblStatus || !status) return;
  setLabelIfChanged(s_lblStatus, s_txtStatus, sizeof(s_txtStatus), status);
}
