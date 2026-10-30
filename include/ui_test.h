// ui_test.h — Spawer op-test LVGL UI (D0–D3 + SP H101 ±0.5 + H13 + START/STOP)
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Snapshot for flicker-free label refresh (main → UI). */
typedef struct UiTestSnap {
  /** D0 Okolní teplota (holding 0). */
  uint16_t ambientRaw;
  uint16_t ambientAddr;
  bool haveAmbient;
  /** D2 Výstupní teplota vody (holding 2). */
  uint16_t outletRaw;
  uint16_t outletAddr;
  bool haveOutlet;
  /** D1 Teplota vstupní vody (holding 1). */
  uint16_t inletRaw;
  uint16_t inletAddr;
  bool haveInlet;
  /** D3 Teplota cívky (holding 3). */
  uint16_t coilRaw;
  uint16_t coilAddr;
  bool haveCoil;
  /** Setpoint (holding 101; FC16 writable). */
  uint16_t spRaw;
  uint16_t spAddr;
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
 * Compat helper: outlet ← h1; keeps last inlet/SP unless updated via uiTestUpdate.
 */
void uiTestUpdateBasic(uint16_t h1, bool haveH1, uint16_t h100, bool haveH100,
                       uint16_t h101, bool haveH101, uint16_t h13, bool haveH13,
                       const char *status);

/** True once after START button press (debounced; blocked while write busy). */
bool uiTestConsumeStartRequest(void);

/** True once after STOP button press (debounced; blocked while write busy). */
bool uiTestConsumeStopRequest(void);

/** True once after SP − (0.5 °C) button. */
bool uiTestConsumeSpDownRequest(void);

/** True once after SP + (0.5 °C) button. */
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
