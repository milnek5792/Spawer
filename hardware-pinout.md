# Waveshare ESP32-S3-Touch-LCD-7B — RS485 pinout (Spawer Round 1)

Round 1 tool: **PlatformIO / PIO Arduino** pasivní sniffer (`spawer_rs485_sniffer/`). ESPHome na této desce **nepoužíváme**.

## Zdroj

- Deska: [ESP32-S3-Touch-LCD-7B (Waveshare Wiki)](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7B)
- FAQ (jasné mapování DI/RO): [docs.waveshare.com FAQ](https://docs.waveshare.com/ESP32-S3-Touch-LCD-7B/FAQ)
- Arduino RS485 demo: `Serial1.begin(..., RX=15, TX=16)`

## LG_Therma

Uživatelská cesta `C:\Users\mnekv\Documents\Arduino\LG_Therma` je **lokální** a na Cloud Agent VM **není**. LG_Therma je předchozí projekt na **stejné** desce (UART/RS485 vzor). Pinout Round 1 je z Waveshare oficiálních materiálů; po zpřístupnění LG_Therma porovnej sketch.

## UART ↔ RS485

| ESP32-S3 pin | Role v kódu | Transceiver | Poznámka |
|--------------|-------------|-------------|----------|
| **GPIO16** | UART **TX** | DI (driver input) | V kódu otevřený; sniffer neposílá |
| **GPIO15** | UART **RX** | RO (receiver output) | Příjem do MCU |
| — | DE/RE | auto | Onboard SP3485, bez GPIO |

Arduino / PlatformIO (Round 1):

```cpp
#define RS485_RX_PIN 15
#define RS485_TX_PIN 16
Serial1.begin(9600, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
// žádný DE/RE pin — auto na Waveshare
```

## Terminace

Onboard dip **RS485 Terminal Resistor** (120 Ω):

- Panel Spaweru zůstává → Waveshare terminace **OFF / NC**
- Jen Waveshare na konci dlouhé linky → ON podle potřeby

## Konektor

RS485 header: **HY2.0 2P** (A / B). Doporučený kabel z balení Waveshare.

Tip: při SILENT / garbage **prohoď A↔B**, pak baud 4800 / 19200 v Serial monitoru snifferu.

## BOOT tlačítko vs GPIO0 (START_PROBE)

Podle [Waveshare docs](https://docs.waveshare.com/ESP32-S3-Touch-LCD-7B):

| Prvek | Role |
|-------|------|
| **GPIO0** | LCD **G3** (RGB data) — **není** volné user GPIO |
| **BOOT** (fyzické) | download mode: držet při připojení USB / flash |
| **RESET** | restart MCU |

Runtime **START_PROBE** proto **nevážeme na BOOT/GPIO0**. Firmware spouští probe **automaticky ~5 s po bootu** (nebo příkaz `start`). Fyzické BOOT = flash / USB-reset only.
