// board_7b.h — Waveshare ESP32-S3-Touch-LCD-7B panel constants
#ifndef BOARD_7B_H
#define BOARD_7B_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fyzický panel */
#define BOARD_PANEL_W 1024
#define BOARD_PANEL_H 600

/** EEZ / Tab5 design (škálujeme na panel) */
#define BOARD_DESIGN_W 1280
#define BOARD_DESIGN_H 720

/**
 * LVGL transform scale: 256 = 100 %.
 * X: 1024/1280 = 0.8 → 204.8 ≈ 205
 * Y: 600/720  ≈ 0.833 → 213.33 ≈ 213
 */
#define BOARD_SCALE_X ((int32_t)((256 * BOARD_PANEL_W) / BOARD_DESIGN_W))
#define BOARD_SCALE_Y ((int32_t)((256 * BOARD_PANEL_H) / BOARD_DESIGN_H))

#ifdef __cplusplus
}
#endif

#endif
