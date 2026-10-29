// ui_test.cpp — Spawer H1/H100/H101 + Start (LG_Therma_7B ui_lvgl_port patterns)
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

lv_obj_t *s_lblTitle = nullptr;
lv_obj_t *s_lblH1 = nullptr;
lv_obj_t *s_lblH100 = nullptr;
lv_obj_t *s_lblH101 = nullptr;
lv_obj_t *s_lblStatus = nullptr;
lv_obj_t *s_btnStart = nullptr;

volatile bool s_startReq = false;

// Cache — avoid invalidate / FB swap when Modbus values unchanged (flicker)
char s_txtH1[48] = "";
char s_txtH100[32] = "";
char s_txtH101[32] = "";
char s_txtStatus[96] = "";
int s_btnColorState = -1;  // -1 unknown, 0 idle(green), 1 active(amber)

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
  // FULL/DIRECT zero-copy: pxMap = RGB framebuffer; draw_bitmap swaps plane (anti-tear)
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

void onStartClicked(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  s_startReq = true;
  // Immediate proof that touch → CLICKED fired (loop then runs H100 write)
  Serial.println(F("H100_START tap (UI CLICKED)"));
}

void setLabelIfChanged(lv_obj_t *lbl, char *cache, size_t cacheCap, const char *text) {
  if (!lbl || !text || !cache) return;
  if (strncmp(cache, text, cacheCap) == 0) return;
  strncpy(cache, text, cacheCap - 1);
  cache[cacheCap - 1] = '\0';
  lv_label_set_text(lbl, cache);
}

void buildScreen() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1116), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  s_lblTitle = lv_label_create(scr);
  lv_label_set_text(s_lblTitle, "Spawer RS485 — H100 Start test");
  lv_obj_set_style_text_color(s_lblTitle, lv_color_hex(0xE8EEF7), 0);
  lv_obj_set_style_text_font(s_lblTitle, &lv_font_montserrat_28, 0);
  lv_obj_align(s_lblTitle, LV_ALIGN_TOP_MID, 0, 24);

  s_lblH1 = lv_label_create(scr);
  lv_label_set_text(s_lblH1, "H1  —  —.- °C");
  strncpy(s_txtH1, "H1  —  —.- °C", sizeof(s_txtH1) - 1);
  lv_obj_set_style_text_color(s_lblH1, lv_color_hex(0x7FDBCA), 0);
  lv_obj_set_style_text_font(s_lblH1, &lv_font_montserrat_40, 0);
  lv_obj_align(s_lblH1, LV_ALIGN_TOP_LEFT, 48, 100);

  s_lblH100 = lv_label_create(scr);
  lv_label_set_text(s_lblH100, "H100  —");
  strncpy(s_txtH100, "H100  —", sizeof(s_txtH100) - 1);
  lv_obj_set_style_text_color(s_lblH100, lv_color_hex(0xFFD166), 0);
  lv_obj_set_style_text_font(s_lblH100, &lv_font_montserrat_40, 0);
  lv_obj_align(s_lblH100, LV_ALIGN_TOP_LEFT, 48, 170);

  s_lblH101 = lv_label_create(scr);
  lv_label_set_text(s_lblH101, "H101  —");
  strncpy(s_txtH101, "H101  —", sizeof(s_txtH101) - 1);
  lv_obj_set_style_text_color(s_lblH101, lv_color_hex(0xA5B4C8), 0);
  lv_obj_set_style_text_font(s_lblH101, &lv_font_montserrat_32, 0);
  lv_obj_align(s_lblH101, LV_ALIGN_TOP_LEFT, 48, 240);

  s_btnStart = lv_button_create(scr);
  lv_obj_set_size(s_btnStart, 420, 160);
  lv_obj_align(s_btnStart, LV_ALIGN_BOTTOM_MID, 0, -48);
  lv_obj_set_style_bg_color(s_btnStart, lv_color_hex(0x1F8A4C), 0);
  lv_obj_set_style_bg_color(s_btnStart, lv_color_hex(0x146338), LV_STATE_PRESSED);
  lv_obj_set_style_radius(s_btnStart, 18, 0);
  lv_obj_add_flag(s_btnStart, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_btnStart, onStartClicked, LV_EVENT_CLICKED, nullptr);
  s_btnColorState = 0;

  lv_obj_t *btnLbl = lv_label_create(s_btnStart);
  lv_label_set_text(btnLbl, "START");
  lv_obj_set_style_text_font(btnLbl, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(btnLbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(btnLbl);
  // Label must not steal clicks from the button
  lv_obj_clear_flag(btnLbl, LV_OBJ_FLAG_CLICKABLE);

  s_lblStatus = lv_label_create(scr);
  const char *st0 = "Start = quiet-bus FC06 H100 (tap; Serial H100_START)";
  lv_label_set_text(s_lblStatus, st0);
  strncpy(s_txtStatus, st0, sizeof(s_txtStatus) - 1);
  lv_obj_set_style_text_color(s_lblStatus, lv_color_hex(0x8A93A3), 0);
  lv_obj_set_style_text_font(s_lblStatus, &lv_font_montserrat_16, 0);
  lv_obj_align(s_lblStatus, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_set_width(s_lblStatus, BOARD_PANEL_W - 80);
  lv_label_set_long_mode(s_lblStatus, LV_LABEL_LONG_WRAP);
}

}  // namespace

bool uiTestInit(void) {
  Serial.println(F("[UI] Waveshare 7B RGB + GT911 + LVGL bring-up…"));

  DEV_I2C_Init();
  IO_EXTENSION_Init();
  IO_EXTENSION_Output(IO_EXTENSION_IO_5, 0);  // native USB, not CAN
  IO_EXTENSION_Output(IO_EXTENSION_IO_3, 0);  // LCD reset pulse
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
  // FULL + double FB: each flush paints complete frame → no DIRECT dual-FB divergence flicker
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
  Serial.printf("[UI] OK %dx%d touch=%s (FULL FB, no per-poll redraw)\n",
                BOARD_PANEL_W, BOARD_PANEL_H, s_touch ? "GT911" : "none");
  return true;
}

void uiTestTick(void) {
  if (!s_ok) return;
  uint32_t now = millis();
  if (now != s_lastTickMs) {
    uint32_t dt = now - s_lastTickMs;
    // Cap single step so a long Modbus block doesn't jump LVGL timers wildly
    lv_tick_inc(dt > 30 ? 30 : dt);
    s_lastTickMs = now;
  }
  lv_timer_handler();
}

void uiTestUpdate(uint16_t h1, bool haveH1, uint16_t h100, bool haveH100,
                  uint16_t h101, bool haveH101, const char *status) {
  if (!s_ok) return;

  char buf[64];
  if (haveH1) {
    int whole = (int)h1 / 10;
    int frac = (int)h1 % 10;
    snprintf(buf, sizeof(buf), "H1  %d.%d °C  (raw %u)", whole, frac, (unsigned)h1);
  } else {
    snprintf(buf, sizeof(buf), "H1  —.- °C");
  }
  setLabelIfChanged(s_lblH1, s_txtH1, sizeof(s_txtH1), buf);

  if (haveH100) {
    snprintf(buf, sizeof(buf), "H100  %u", (unsigned)h100);
  } else {
    snprintf(buf, sizeof(buf), "H100  —");
  }
  setLabelIfChanged(s_lblH100, s_txtH100, sizeof(s_txtH100), buf);

  if (haveH101) {
    snprintf(buf, sizeof(buf), "H101  %u", (unsigned)h101);
  } else {
    snprintf(buf, sizeof(buf), "H101  —");
  }
  setLabelIfChanged(s_lblH101, s_txtH101, sizeof(s_txtH101), buf);

  if (status && *status) {
    setLabelIfChanged(s_lblStatus, s_txtStatus, sizeof(s_txtStatus), status);
  }

  // Button color only when H100 state changes (avoids FB swap every poll)
  if (s_btnStart && haveH100) {
    int want = h100 ? 1 : 0;
    if (want != s_btnColorState) {
      s_btnColorState = want;
      lv_obj_set_style_bg_color(
          s_btnStart,
          want ? lv_color_hex(0xC47A16) : lv_color_hex(0x1F8A4C), 0);
    }
  }
}

bool uiTestConsumeStartRequest(void) {
  if (!s_startReq) return false;
  s_startReq = false;
  return true;
}

void uiTestSetStatus(const char *status) {
  if (!s_ok || !s_lblStatus || !status) return;
  setLabelIfChanged(s_lblStatus, s_txtStatus, sizeof(s_txtStatus), status);
}
