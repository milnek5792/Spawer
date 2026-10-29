// ui_test.h — Spawer op-test LVGL UI (temps + SP? + H13 + START/STOP)
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Snapshot for flicker-free label refresh (main → UI). */
typedef struct UiTestSnap {
  uint16_t h1;
  bool haveH1;
  /** H2..H5 raw (Fairland-like nearby temps / setpoint candidates). */
  uint16_t nearRaw[4];
  bool haveNear[4];
  /** Chosen setpoint candidate (addr 2..5 typical); 0 = none. */
  uint16_t spAddr;
  uint16_t spRaw;
  bool haveSp;
  uint16_t h100;
  bool haveH100;
  uint16_t h101;
  bool haveH101;
  /** Holding 13 status/bits (DIFFAULT: H13=64 → bit6 ≈ panel error 6). */
  uint16_t h13;
  bool haveH13;
  const char *status;
} UiTestSnap;

/** Bring up I2C / IO ext / RGB LCD / GT911 / LVGL + Spawer op-test screen. */
bool uiTestInit(void);

/** Call from loop — lv_tick + lv_timer_handler. */
void uiTestTick(void);

/** Refresh labels (only when text/state changes — no flicker). */
void uiTestUpdate(const UiTestSnap *snap);

/**
 * Compat helper used by older call sites / quick updates.
 * Near/SP left unchanged unless you also call uiTestSetNear.
 */
void uiTestUpdateBasic(uint16_t h1, bool haveH1, uint16_t h100, bool haveH100,
                       uint16_t h101, bool haveH101, uint16_t h13, bool haveH13,
                       const char *status);

/** Update H2–H5 + SP? pick (keeps last H1/H100/H101/H13). */
void uiTestSetNear(const uint16_t nearRaw[4], const bool haveNear[4],
                   uint16_t spAddr, uint16_t spRaw, bool haveSp);

/** True once after START button press. */
bool uiTestConsumeStartRequest(void);

/** True once after STOP button press. */
bool uiTestConsumeStopRequest(void);

/** Optional: set status line (TX result etc.). */
void uiTestSetStatus(const char *status);

#ifdef __cplusplus
}
#endif
