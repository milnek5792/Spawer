# Spawer RS485 — návod (Round 1)

## 1. Co to dělá

Firmware na **Waveshare ESP32-S3-Touch-LCD-7B** (UART **RX=GPIO15**, **TX=GPIO16**, auto DE/RE):

### Aktuální cíl: H100 Start (panel 1→0) + LCD test UI

1. **Quiet Serial (default)** — **`H100=` / `H1=` / `H101=`** (+ **`CHANGE`**), quiet **`H7`/`H12`/`H13`/`H20`/`H23` CHANGE**, **`H13=N (0x..) bits=B`**, **`H100 busy/ex6`**, **`H100_START`**, **`SCAN6` hits**, `alive` / `SILENT`.
2. **FC03 fast watch** — slave **1**, **9600 8N1**, každých **~1 s** (priorita **H100**):
   - **holding 100–101** (blok qty 2) → **`H100=N`** / **`H101=N`** (při OK + LCD)
   - **Modbus EXC 0x06** (Slave Device Busy): **ne** mazat last good / UI „—“; **2–3× retry** + backoff ~200 ms; log **`H100 busy/ex6`**
   - settle, pak **holding 1** → **`H1=N`**; na LCD **°C = raw × 0.1**
   - settle, **holding 7–23** → quiet **`H7`/`H12`/`H13`/`H20`/`H23`** (**CHANGE**); **`H13=64 bits=6`** ≈ panel **error 6**
   - **H0** volitelně **OFF**; auto temp/error scan **OFF**
   - **Milan:** panel **Start** → **`H100=… CHANGE`** (**1→0**) → auto **SCAN6**
3. **LCD test UI** (LVGL): **H1 (°C)**, **H100**, **H101**, **H13 hex/bits** + **START** → **quiet-bus + FC16 qty1 holding 100**. Po Start status = H13. Serial: `h100` / `fc16`. **FC06 nic nedělá.**
4. **SCAN6** — FC03 raw **6** 0–2047. Auto po FC16 / H100 1→0; ručně **`scan6`**.
5. **SCANFC** — **Serial-only** (`scanfc` / `fcscan`); **ne auto** (blokuje UI). FC04/01/02 hunt.
6. **DIFFAULT** — okna; 1. `diffault` = baseline, 2. = `DIFF_ADDR=`. Auto před/po FC16.
7. **Temp/error scan** — jen ručně `poll` (+ `verbose`). **START_PROBE** auto OFF (`start`).

Hledej: **`H13=` / `bits=`**, **`H100=` / `CHANGE`**, **`DIFF_ADDR=`**, **`H100_START`**.

> **Milan — REFLASH** (H13 bits; Start = FC16; SCANFC = Serial-only):
> ```bash
> cd …/docs/spawer-rs485/spawer_rs485_sniffer
> pio run -t upload
> pio device monitor -e waveshare_esp32s3_lcd7b
> ```
> Po Start očekávej **`H13=64 (0x40) bits=6`** (+ LCD). Ručně: `scanfc` / `scan6` / `diffault` / `fc16`.

### Live FC03 hodnoty (Milan) — jen raw

| Reg | raw (příklad) | Poznámka |
|-----|---------------|----------|
| 0 | **200–210** | **Není** 0/1 power — spíš režim/setpoint; fast poll **OFF** |
| 1 | **190–200** | fast poll `H1=` · LCD **×0.1 °C** (19.0–20.0) |
| 2–3 | **180–190** | |
| 4–6, 9–11, 13–15 | často **0** | kandidáti START_PROBE FC06 |
| 7–8 | **190–200** | |
| 12 | **0** nebo **8** | |
| **100** | **0 / 1** | **Start watch** — panel Start → **CHANGE 1→0**; UI FC06 write |
| 101 | **?** | fast poll `H101=` |

FC04 / FC01 / FC02 **neposílají se** (timeout šum — scan je jen FC03).

### ⚠️ START / H100 zápis — varování

- **UI Start / `h100` / `fc16`** = pause FC03 → flush RX → **≥80 ms quiet bus** → **FC16 qty1 holding[100]** (toggle) → echo → settle **≥800 ms** → resume poll. Vždy plný **TX/RX hex**.
- **FC06 na této Spawer pumpě nic nedělá** — primary Start je **jen FC16** (ne režimový přepínač).
- Panel ukázal **H100 1→0** (pak auto-clear); zápis **1** může být špatně — firmware proto **píše 0 když H100==1**, jinak **1**; po zápisu **nebojuje** auto-clear.
- **`h100alt`** = experiment: **FC16 qty1=1 / =0**, pak FC06 srovnání — u každého `ACK` / `EXC6` / `TIMEOUT`.
- Starý **START_PROBE** (`start`) dál zkouší kandidáty v **0–31** — oddělené od H100.
- **holding[0]=1 jako „start“ je nepotvrzené** — hodnota ~200 při běhu ukazuje spíš režim.
- **Chyba na panelu Spaweru = jednotka už běží** — Modbus „start“ nemusí nic viditelně udělat.
- **BOOT tlačítko 7B** = download / USB-reset. **GPIO0 = LCD G3** → runtime START = **LCD Start** / `h100` / `fc16` / `h100alt` / `start`, ne fyzické BOOT.

## 2. Co potřebuješ

| Položka | Poznámka |
|--------|----------|
| Waveshare ESP32-S3-Touch-LCD-7B | USB-C napájení + flash; LCD test UI |
| PlatformIO (`pio`) | stejný přístup jako LG Therma (USB CDC) |
| Kabel RS485 A/B | HY2.0 2P na Waveshare |
| Spawer panel | paralelní A/B; terminace Waveshare **OFF** |
| (volitelně) USB-RS485 dongle | ověření TX path před Spawerem |

Pinout: [`hardware-pinout.md`](hardware-pinout.md) · firmware: [`spawer_rs485_sniffer/`](spawer_rs485_sniffer/)

## 3. Flash / reflash (PIO)

Po změně firmware (**LCD UI / H100 Start**) **přehrad** desku:

```bash
cd …/docs/spawer-rs485/spawer_rs485_sniffer
pio run -t upload
```

V PIO Arduino / VS Code: **Open folder** → `spawer_rs485_sniffer/` → **Upload** (env `waveshare_esp32s3_lcd7b`).

1. USB-C do Waveshare, COM v Device Manageru.
2. Upload dokud PIO nehlásí success.
3. Po flashi **zavři a znovu otevři** monitor (Windows CDC po resetu často „ztratí“ port).
4. V boot logu ověř: `[UI] OK 1024x600`, `H1=` / `H100=` / `H101=`, `H100_START` po Start.

USB flags: `ARDUINO_USB_CDC_ON_BOOT=1`, `ARDUINO_USB_MODE=1`, monitor **115200**.

## 4. USB monitor

```bash
pio device monitor -e waveshare_esp32s3_lcd7b
```

1. Ve Windows Device Manageru vyber COM desky („USB Serial Device“ / CDC).
2. **Jeden** monitor najednou.
3. Po **hardwarovém resetu** monitor **zavři a znovu otevři**.
4. Firmware čeká max ~3 s na CDC; každých **30 s** píše `alive`.
5. **PuTTY často nepřijímá vstup** — poll běží sám; `h100` / `start` přes `pio device monitor`.

## 5. Spawer — hlavní test (H100 Start + LCD)

```
Spawer RS485 A  ───  Waveshare RS485 A
Spawer RS485 B  ───  Waveshare RS485 B
GND (volitelně) ───  společná zem
```

1. Zapoj Spawer A/B (paralelní napojení na panel).
2. Dip **RS485 termination** na Waveshare: **OFF / NC**.
3. **Reflash** + otevři monitor @ 115200 (`pio device monitor`).
4. LCD: **H1 °C**, **H100**, **H101**, **H13 hex/bits**; Serial = `H100=` / `H1=` / `H101=` + quiet **`H7`/`H12`/`H13`/`H20`/`H23` CHANGE**.
5. **LCD START** / `fc16` — FC16 + confirm + **H13 force** + SCAN6 + DIFFAULT (**SCANFC ne auto**).
6. **Panel Start** — **`H100=… CHANGE` 1→0** → auto **SCAN6**; sleduj **`H13=… bits=`**.
7. Ručně: **`scanfc`** (Serial-only), **`scan6`**, **`diffault`**.
8. Pošli `H13=` / `bits=` / `DIFF_ADDR=` / `CHANGE` řádky.

**Očekávání:** po Start **`H13=64 bits=6`** na Serial + LCD (hypotéza = panel error 6).

**Tip při samých TIMEOUT:** prohoď **A↔B**, ověř napájení Spaweru, pak baud `4800` / `19200`.

## 6. Ověření RS485 HW (volitelné, před Spawerem)

**Nezkratuj A s B** — to není UART loopback.

### A — echo příkazem

Auto echo **vypnuté**. V monitoru: `echo` → `NO_LOCAL_ECHO` / `LOCAL_ECHO`.  
`NO_LOCAL_ECHO` = OK half-duplex.

### B — USB-RS485 dongle

```
Waveshare A/B  ───  dongle A/B  (+GND)
```

Obě strany **9600 8N1**. `echo` / `tx` → na donglu `E5 53 50 41 57 45 52 0A`.

## 7. Příkazy v monitoru

USB 115200, příkaz + Enter (**volitelné**):

| Příkaz | Akce |
|--------|------|
| *(auto fast)* | FC03 H1 + 100–101 + **500–520** ~1 s → `H1=` / `H100=` [/ `CHANGE`]; H5xx jen CHANGE/nonzero |
| *(LCD Start / panel 1→0)* | po FC16 confirm nebo H100 **1→0** → **`SCAN6`** (`ERR6_ADDR=` / `Hnnn=6`) |
| `h100` / `start100` / `fc16` | totéž jako LCD Start (**FC16 only**; bez tap řádku) + SCAN6 |
| `fc16 0` / `fc16 1` | FC16 qty1 s explicitní hodnotou |
| `scan6` / `err6` | ruční FC03 SCAN6 raw=6 (0–2047 qty16; opakovatelné) |
| `errdump` / `err` | alias **`scan6`** |
| `h100alt` | FC16 first, pak FC06 experiment — log `ACK` / `EXC6` / `TIMEOUT` |
| `fc06` | experiment FC06 (na této pumpě typicky **nic**) |
| *(auto scan)* | OFF — ručně `poll` → `TEMP_ADDR` / `ERROR_ADDR` |
| `poll` / `p` | jeden FC03 temp/error scan hned |
| `quiet` / `q` | jen `H1/H100/H101` + busy/ex6 + H100_START + SCAN6 triggers (default) |
| `verbose` / `v` | dump bloků hex + TEMP / ERROR + pasivní frames |
| `start` / `s` | START_PROBE FC06 kandidáti 0–31 (**auto OFF**) |
| `pollon` / `nopol` | zapni / vypni fast + scan |
| `help` | nápověda |
| `listen` / `l` | připomínka režimu |
| `b` / `baud` | aktuální bus baud |
| `9600` … `115200` | přepni bus baud |
| `echo` / `e` / `ping` | TX test (ne na živém Spawer, pokud nemusíš) |
| `echoon` / `echooff` | auto echo 3× po startu (default **off**) |
| `tx` / `loop` | jednorázový TX / návod A/B |
| BOOT (fyzické) | **download / USB-reset** — **ne** runtime START (GPIO0=LCD G3) |

## 8. Co znamenají výsledky

| Tag / výsledek | Význam |
|----------------|--------|
| `H1=N` | FC03 holding 1 = N (~0.75 s); LCD °C = N×0.1 |
| `H100=N` / `H101=N` | FC03 holding 100 / 101 (~0.75 s, jeden blok) |
| `H100=N CHANGE` | holding 100 se změnil — **panel Start → typicky 1→0** |
| `H100_START tap` | LCD CLICKED dorazil (touch OK) |
| `H100_START TX/RX` | quiet-bus **FC16** qty1 (Start primary) — hex + `result=ACK\|EXC6\|TIMEOUT` |
| `MODBUS_OK` | aktivní čtení/zápis OK — Spawer odpovídá na RTU |
| `MODBUS_TIMEOUT` | žádná odpověď — **A↔B**, napájení, baud, slave≠1 / mimo mapu |
| `TEMP_ADDR holding=N raw=V` | temp hit — `N` = abs holding, raw **150..350** |
| `TEMP_ADDR none` | v pásmu 150–350 nic (nebo samé TO) |
| `ERR6_ADDR=N` / `HN=6` | SCAN6 hit — holding **N** má raw **6** (fault latch) |
| `SCAN6 …` | plný FC03 sweep 0–2047 qty16; auto po Start / ručně `scan6` |
| `ERROR_ADDR holding=N raw=6` | jen **verbose** / starý 10–20 scan |
| `MODBUS_EXC` | výjimka od slave — bus/Modbus žije, mapování jiné |
| `MODBUS_CRC_ERR` | bajty přišly, CRC nesedí — šum / polarita / baud |
| `START_PROBE TX/RX` | jen po `start` — FC06 kandidáti 0–31 |
| `data: [n]=…` | jen **verbose** / START_PROBE — FC03 raw |
| `MODBUS` (pasivní) | cizí rámec vypadá jako RTU |
| `PROPRIETARY_80B` | hlavičky `DD`/`D2`/`CC`/`CD`, ~80 B |
| `SILENT` | žádná data ~10 s (pasivní) |
| `NO_LOCAL_ECHO` | očekávané u `echo` (half-duplex) |

**Verdikt Round 1:** aspoň jeden `MODBUS_OK` = Spawer mluví Modbus RTU.  
FC03 potvrzeno. **H100** = Start watch kandidát (panel **1→0**). Power přes holding[0] nepotvrzen.

## 9. Časté problémy

| Problém | Co zkusit |
|---------|-----------|
| Samé `MODBUS_TIMEOUT` | **Prohoď A↔B**, napájení Spaweru, `4800`/`19200` |
| LCD černý / bez UI | Reflash (nový platform + LVGL); EXIO USB=0; jas přes IO ext |
| Touch nereaguje | GT911 I2C 8/9; boot log `[UI] … touch=GT911` |
| FC04/01 se neposílají | očekávané — scan jen FC03 |
| BOOT „nereaguje“ na START | **normální na 7B** — GPIO0=LCD G3; Start = LCD / `h100` |
| Prázdný USB monitor | Znovu otevři COM po resetu; CDC flags |
| PuTTY bez vstupu | Normální — poll běží sám; `h100`/`start` přes `pio device monitor` |
| Panel ukazuje chybu | Jednotka **už běží** — zápis nemusí nic změnit |
| Rušení Spaweru echo patternem | Auto echo OFF; `echo`/`tx` jen vědomě |
| Zkrat A↔B jako „loopback“ | **Nedělej** |

---

### Soubory

```
docs/spawer-rs485/
├── README.md                 ← tento návod
├── hardware-pinout.md
├── spawer_rs485_sniffer/     ← OTEVŘI TUTO SLOŽKU (PIO)
│   ├── platformio.ini
│   ├── include/              ← lv_conf.h, board_7b.h, ui_test.h
│   └── src/
│       ├── main.cpp
│       ├── ui_test.cpp       ← LVGL test UI
│       └── board/            ← Waveshare RGB/GT911 (z LG_Therma_7B)
└── sniffer/                  ← přesměrování (starý layout)
```

### Mimo Round 1

Plné řídicí UI, MQTT/HA, ověřené power/mode registry mimo H100 experiment.
