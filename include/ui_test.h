// ui_test.h — minimal LVGL test UI on Waveshare ESP32-S3-Touch-LCD-7B
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up I2C / IO ext / RGB LCD / GT911 / LVGL + Spawer test screen. */
bool uiTestInit(void);

/** Call from loop — lv_tick + lv_timer_handler. */
void uiTestTick(void);

/**
 * Refresh labels from last Modbus values.
 * H1 shown as °C (raw × 0.1). H100 / H101 as raw ints.
 * H13 as hex + set bits (DIFFAULT: H13=64 → bits=6 ≈ panel error 6).
 */
void uiTestUpdate(uint16_t h1, bool haveH1, uint16_t h100, bool haveH100,
                  uint16_t h101, bool haveH101, uint16_t h13, bool haveH13,
                  const char *status);

/** True once after Start button press (or long-press toggle request). */
bool uiTestConsumeStartRequest(void);

/** Optional: set status line (TX result etc.). */
void uiTestSetStatus(const char *status);

#ifdef __cplusplus
}
#endif
