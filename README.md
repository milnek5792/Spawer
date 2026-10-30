# Spawer RS485 sniffer (PIO)

Firmware for **Waveshare ESP32-S3-Touch-LCD-7B** — Modbus RTU master on RS485 (**RX=GPIO15**, **TX=GPIO16**) + LVGL op-test UI.

**Project home (next to manual):**  
`C:\Users\mnekv\Documents\Arduino\Spawer_Ospa\spawer_rs485_sniffer`  
Manual dump: `..\MANUAL_POOLEX_OSPA_param.pdf`

## Manual map (table D = holding from address 0)

| Holding | Manual | LCD / Serial |
|--------:|--------|--------------|
| 0 | **D0** Okolní teplota | `D0=` / D0 Okolni |
| 1 | **D1** Teplota vstupní vody | `D1=` / D1 Vstup |
| 2 | **D2** Výstupní teplota vody | `D2=` / D2 Vystup |
| 3 | **D3** Teplota cívky | `D3=` / D3 Civka |
| 4–9 | D4…D9 (kompresor / ventilátor / …) | polled in 0–5 block |
| **100** | Start/Stop | `H100=` — **FC16** START=1 / STOP=0 |
| **101** | **Setpoint** (not table D) | `SP=` — **FC16** ±0.5 °C (raw×0.1) |

## Quiet Serial / LCD

- Tags: **`D0=` / `D1=` / `D2=` / `D3=` / `SP=` / `H100=`** (+ `CHANGE`), **`H13=… bits=`**, **`H100_START` / `H100_STOP` / `SP_SET`**
- LCD: D1 Vstup, D2 Vystup, D0 Okolni, D3 Civka, **SP H101 ±**, H100, H13, START/STOP
- Busy unlock (PR #7): write-busy released after short H100 confirm — SCAN6/DIFFAULT/SCANFC = Serial-only

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
| *(auto ~1 s)* | FC03 H100–101 (Start + SP) + D0–D5 + DIFF + H500–520 |
| `h100` / `fc16` | START FC16 H100=1 |
| `stop` / `fc16 0` | STOP FC16 H100=0 |
| `sp+` / `sp-` / `sp <raw>` | setpoint FC16 **H101** |
| `scan6` / `scanfc` / `diffault` | Serial-only diagnostics |
| `quiet` / `verbose` / `help` | Serial mode |

## Layout

```
Spawer_Ospa/
├── MANUAL_POOLEX_OSPA_param.pdf
└── spawer_rs485_sniffer/     ← this PIO project (git → milnek5792/Spawer)
    ├── platformio.ini
    ├── include/
    └── src/main.cpp          ← D map + SP=101 + H100 Start/Stop
```
