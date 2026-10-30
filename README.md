# Spawer RS485 sniffer (PIO)

Firmware for **Waveshare ESP32-S3-Touch-LCD-7B** — Modbus RTU master on RS485 (**RX=GPIO15**, **TX=GPIO16**) + LVGL op-test UI.

**Project home (next to manual):**  
`C:\Users\mnekv\Documents\Arduino\Spawer_Ospa\spawer_rs485_sniffer`  
Manual dump: `..\MANUAL_POOLEX_OSPA_param.pdf`

## Manual map

| Holding | Manual | Scale / notes |
|--------:|--------|---------------|
| 0 | **D0** ambient | **raw×0.1** °C |
| **1** | **D1** inlet water | **Integer °C** (not ×0.1 — Milan) |
| 2 | **D2** outlet | **raw×0.1** °C |
| 3 | **D3** coil | **raw×0.1** °C |
| 4..9 | **D4..D9** status | ZAP/VYP / status |
| **99** | **C0** heating setpoint (SP) | **Integer °C** (15–40); FC16 ±1 — **try H99** (Milan) |
| **100** | **Start/Stop** | **FC16** START=1 / STOP=0 — **KEEP** |
| **101** | **C1** hysteresis (dT restart) | Integer °C — **NOT SP** (Milan) |
| 102..111 | **C2..C11** | Integer °C / 0–1 flags |

PDF has **no Modbus addresses**. Map is project convention + Milan live feedback.  
**Start stays on H100.** Previous SP@H101 was wrong (H101 = hysteresis).

## Quiet Serial / LCD

- Tags: **`D0=`…`D9=` / `C0=`…`C11=` / `SP=` / `H100=`** (+ `CHANGE`), **`H13=… bits=`**, **`H100_START` / `H100_STOP` / `SP_SET`**
- LCD: **D0…D9** (D1 int) + **C0…C11** + **C0/SP H99 ±1 °C** + H100 + H101 hyst + H13 + START/STOP

## Flash

```bash
cd C:\Users\mnekv\Documents\Arduino\Spawer_Ospa\spawer_rs485_sniffer
pio run -e waveshare_esp32s3_lcd7b
pio run -e waveshare_esp32s3_lcd7b -t upload
pio device monitor -e waveshare_esp32s3_lcd7b
```

Pinout: [`hardware-pinout.md`](hardware-pinout.md)

## Commands (USB 115200)

| Command | Action |
|---------|--------|
| *(auto ~1 s)* | FC03 H99–111 (SP+Start+C) + **D0–D9** + DIFF + H500–520 |
| `h100` / `fc16` | START FC16 H100=1 |
| `stop` / `fc16 0` | STOP FC16 H100=0 |
| `sp+` / `sp-` / `sp <C>` | C0/SP FC16 **H99** ±1 °C integer (e.g. `sp 28`) |
| `scan6` / `scanfc` / `diffault` | Serial-only diagnostics |
| `quiet` / `verbose` / `help` | Serial mode |

## Layout

```
Spawer_Ospa/
├── MANUAL_POOLEX_OSPA_param.pdf
└── spawer_rs485_sniffer/     ← this PIO project (git → milnek5792/Spawer)
    ├── platformio.ini
    ├── include/
    └── src/main.cpp
```
