# Spawer RS485 sniffer (PIO)

Firmware for **Waveshare ESP32-S3-Touch-LCD-7B** — Modbus RTU master on RS485 (**RX=GPIO15**, **TX=GPIO16**) + LVGL op-test UI.

**Project home (next to manual):**  
`C:\Users\mnekv\Documents\Arduino\Spawer_Ospa\spawer_rs485_sniffer`  
Manual dump: `..\MANUAL_POOLEX_OSPA_param.pdf`

## Manual map

| Holding | Manual | Scale / notes |
|--------:|--------|---------------|
| 0..9 | **D0..D9** status | D0–D3 measured temps **raw×0.1** °C (live 200→20.0); D4–D9 ZAP/VYP |
| **100** | **Start/Stop** | **FC16** START=1 / STOP=0 — **KEEP** (works in practice) |
| **101** | **C0** heating setpoint (SP) | **Integer °C** (manual C0 15–40); **not** ×0.1 (raw 2 → **2**, not 0.2) |
| 102..112 | **C1..C11** | Integer °C / 0–1 flags per manual C |

PDF has **no Modbus addresses**. Map above is project convention: D from 0; H100 reserved for Start; C0.. at 101+.  
Claim “SP = H100” **conflicts** with live working Start on H100 — **Start stays on 100**.

## Quiet Serial / LCD

- Tags: **`D0=`…`D9=` / `C0=`…`C11=` / `SP=` / `H100=`** (+ `CHANGE`), **`H13=… bits=`**, **`H100_START` / `H100_STOP` / `SP_SET`**
- LCD: **D0…D9** + **C0…C11** + **C0/SP H101 ±1 °C** + H100 + H13 + START/STOP

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
