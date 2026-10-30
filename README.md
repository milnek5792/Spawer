# Spawer RS485 sniffer (PIO)

Firmware for **Waveshare ESP32-S3-Touch-LCD-7B** — Modbus RTU master on RS485 (**RX=GPIO15**, **TX=GPIO16**) + LVGL op-test UI.

**Project home (next to manual):**  
`C:\Users\mnekv\Documents\Arduino\Spawer_Ospa\spawer_rs485_sniffer`  
Manual dump: `..\MANUAL_POOLEX_OSPA_param.pdf`

## Manual map

| Holding | Manual | Scale / notes |
|--------:|--------|---------------|
| 0 | **D0** ambient | **raw×0.1** °C (live Modbus; manual does **not** state tenths) |
| **1** | **D1** inlet water | **Integer °C** (Milan re-verify — ×0.1 looked wrong) |
| 2 | **D2** outlet | **raw×0.1** °C (live) |
| 3 | **D3** coil | **raw×0.1** °C (live) |
| 4..9 | **D4..D9** status | ZAP/VYP / status |
| **100** | **Start/Stop** | **FC16** START=1 / STOP=0 — **KEEP** |
| **101** | **C0** heating setpoint (**SP**) | **Integer °C** (15–40); **FC16 ± only here** |
| **102** | **C1** hysteresis (dT restart) | Integer °C — **NOT SP** (do not ±) |
| 103..112 | **C2..C11** | Integer °C / 0–1 flags |

PDF has **no Modbus addresses** and **no 0.1 °C wire resolution** for D0–D3 (only °C ranges as measured).  
C-table setpoints in the manual are **whole °C**. Wire D0/D2/D3×0.1 is live RS485 evidence (raw 200≈20.0).  
**C grid:** C0=H101, C1=H102, … C11=H112. **Start = H100.** **SP label + FC16 ± = C0/H101 only.**  
FC03 block 100–112 → absorb `C[i]=vals[1+i]` (C0←vals[1]=H101). Never `vals[i]` (off-by-one → SP=H102). Serial: `H101= C0= SP=` must match.

## Quiet Serial / LCD

- Tags: **`D0=`…`D9=` / `C0=`…`C11=` / `SP=` / `H100=`** (+ `CHANGE`), **`H13=… bits=`**, **`H100_START` / `H100_STOP` / `SP_SET`**
- LCD: **D** with scale tags (`×.1` / `int`) + **C0 H101 … C11 H112** + **SP=C0 H101 ± FC16** (buttons **SP−/SP+**) + H100 + H13 + START/STOP

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
| *(auto ~1 s)* | FC03 H100–112 (Start+C) + **D0–D9** + DIFF + H500–520 |
| `h100` / `fc16` | START FC16 H100=1 |
| `stop` / `fc16 0` | STOP FC16 H100=0 |
| `sp+` / `sp-` / `sp <C>` | C0/SP FC16 **H101** ±1 °C integer (e.g. `sp 28`) |
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
