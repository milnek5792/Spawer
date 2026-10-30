// ui_test.h — Spawer op-test LVGL UI (D0–D9 + C0–C11 + SP H101 int °C + H100 Start + H13)
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Manual table D count (holding 0..9). */
#define UI_D_COUNT 10

/**
 * Manual table C subset shown on LCD (C0..C11).
 * Holding map (project): H100 = Start/Stop (NOT a C param);
 * C0..C11 → holding 101..112 (C0 = SP).
 */
#define UI_C_COUNT 12

/** Snapshot for flicker-free label refresh (main → UI). */
typedef struct UiTestSnap {
  /** D0..D9 raw holdings (manual table from address 0). */
  uint16_t dRaw[UI_D_COUNT];
  bool haveD[UI_D_COUNT];
  /** C0..C11 raw holdings (integer °C / 0-1 flags per manual C). */
  uint16_t cRaw[UI_C_COUNT];
  bool haveC[UI_C_COUNT];
  /** Setpoint = C0 = holding 101; integer °C (manual C0), FC16 writable. */
  uint16_t spRaw;
  uint16_t spAddr;
  bool haveSp;
  /** Start/Stop holding 100 (FC16); keep — do not relocate. */
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
 * Compat helper: sets D1 from h1; keeps last D/C/SP unless updated via uiTestUpdate.
 */
void uiTestUpdateBasic(uint16_t h1, bool haveH1, uint16_t h100, bool haveH100,
                       uint16_t h101, bool haveH101, uint16_t h13, bool haveH13,
                       const char *status);

/** True once after START button press (debounced; blocked while write busy). */
bool uiTestConsumeStartRequest(void);

/** True once after STOP button press (debounced; blocked while write busy). */
bool uiTestConsumeStopRequest(void);

/** True once after SP − (1 °C integer, manual C0) button. */
bool uiTestConsumeSpDownRequest(void);

/** True once after SP + (1 °C integer, manual C0) button. */
bool uiTestConsumeSpUpRequest(void);

/**
 * Block/allow new taps while FC16 (H100 or SP) is in flight.
 * Also clears pending start/stop/SP flags when busy=true.
 */
void uiTestSetWriteBusy(bool busy);

/** Optional: set status line (TX result etc.). */
void uiTestSetStatus(const char *status);

#ifdef __cplusplus
}
#endif
