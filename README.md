# Spawer RS485 — návod (Round 1)

## 1. Co to dělá

Firmware na **Waveshare ESP32-S3-Touch-LCD-7B** (UART **RX=GPIO15**, **TX=GPIO16**, auto DE/RE):

### Aktuální cíl: **op-test LCD** — IN / OUT / SP ±0.5 + START/STOP

1. **Quiet Serial (default)** — **`OUT=` / `IN=` / `SP=` / `H100=` / `H101=`** (+ **`CHANGE`**), quiet **`H7`/`H12`/`H13`/`H20`/`H23` CHANGE**, **`H13=N bits=B`**, **`H100_START` / `H100_STOP` / `SP_SET`**, `alive` / `SILENT`.
2. **FC03 fast watch** — slave **1**, **9600 8N1**, každých **~1 s** (priorita **H100**):
   - **holding 100–101** → **`H100=` / `H101=`**
   - settle, **holding 0–5** → **`OUT=` / `IN=` / `SP=`** (raw **×0.1 °C**)
   - settle, **holding 7–23** → **`H13` bits** (DIFF; **H13=64 bits=6** ≈ panel error 6)
   - settle, **500–520** quiet CHANGE/nonzero
3. **LCD op-test UI** (LVGL, no flicker): **OUT**, **IN**, **SP** s **− / +** (**0.5 °C**, FC16), **H100/H101**, **H13 bits** + velké **START** (**FC16 H100=1**) a **STOP** (**FC16 H100=0**).
4. **START/STOP harden** — quiet-bus **≥200 ms**, až **3×** FC16 při **EXC6/TIMEOUT**, FC03 confirm, LCD status, touch debounce + **in-flight lock** (druhý zápis se zahodí).
5. **SCAN6** / **SCANFC** (Serial-only) / **DIFFAULT** — beze změny logiky H13.

Hledej: **`OUT=` / `IN=` / `SP=`**, **`H13=` / `bits=`**, **`H100_START` / `H100_STOP` / `SP_SET`**.

### Remap konstant (když panel nesedí)

V `src/main.cpp` (0-based holding, raw×0.1 = °C):

```cpp
static const uint16_t MB_OUTLET_ADDR = 1;  // OUT — default H1 (water/outlet-ish)
static const uint16_t MB_INLET_ADDR  = 2;  // IN  — default H2 (pair with H1)
static const uint16_t MB_SP_ADDR     = 0;  // SP  — default H0 (writable candidate)
static const uint16_t MB_SP_STEP_RAW = 5;  // 0.5 °C
```

Pokud Milan na panelu uvidí jiné párování (např. OUT=H8, SP=H3), stačí přepsat tyto tři adresy a přeflashovat. Poll blok **H0–H5** musí pokrývat zvolené adresy (nebo rozšířit `MB_TEMP_BLOCK_*`).

> **Milan — REFLASH (op test)**:
> ```bash
> cd …/docs/spawer-rs485/spawer_rs485_sniffer
> pio run -t upload
> pio device monitor -e waveshare_esp32s3_lcd7b
> ```
> LCD: **OUT / IN / SP± / H100 / H13 / START / STOP**. Serial quiet. Po Start očekávej **`H13=… bits=`**. Ručně: `stop` / `fc16 0` / `sp+` / `scanfc`.

### Live FC03 hodnoty (Milan) — jen raw

| Reg | raw (příklad) | Poznámka |
|-----|---------------|----------|
| **0** | **200–210** | **SP candidate** (writable; LCD SP default) |
| **1** | **190–200** | **OUT candidate** · LCD **×0.1 °C** |
| **2** | **180–190** | **IN candidate** (pair s H1) |
| 3 | **180–190** | často = H2; alternativní SP? |
| 4–5 | často 0 / temp-like | |
| 7–8 | **190–200** | DIFF watch |
| 12 | **0** nebo **8** | DIFF watch |
| **13** | **64** (bit6) | DIFF ≈ panel error 6 |
| **100** | **0 / 1** | **START=1 / STOP=0** (FC16); panel Start → **CHANGE 1→0** |
| 101 | **?** (často 2) | fast poll `H101=` |

FC04 / FC01 / FC02 **neposílají se** (timeout šum — scan je jen FC03). Fairland input 3/4 (inlet/outlet) na Spaweru timeout — teploty žijí v holdings.

### ⚠️ START / H100 zápis — varování

- **UI START / `h100` / `fc16`** = quiet-bus **≥200 ms** → **FC16 H100=1** (až **3 pokusy** při EXC6/TO) → dual FC03 confirm. **UI STOP / `stop` / `fc16 0`** = **FC16 H100=0**. Plný **TX/RX hex** + LCD status.
- **FC06 na této Spawer pumpě nic nedělá** — Start/Stop/SP = **jen FC16**.
- Panel často ukáže **H100 1→0** (auto-clear) — OK; LCD má oddělené START/STOP (ne toggle).
- Touch: **debounce ~450 ms** + **write in-flight lock** — druhý tap během zápisu se ignoruje.
- **`h100alt`** = experiment: **FC16 qty1=1 / =0**, pak FC06 srovnání — u každého `ACK` / `EXC6` / `TIMEOUT`.
- Starý **START_PROBE** (`start`) dál zkouší kandidáty v **0–31** — oddělené od H100.
- **holding[0]=1 jako „start“ je nepotvrzené** — hodnota ~200 při běhu = SP candidate.
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

Pinout: [`hardware-pinout.md`](hardware-pinout.md) · firmware: tento repo root (PIO)

## 3. Flash / reflash (PIO)

Po změně firmware (**LCD UI / H100 Start / SP**) **přehrad** desku:

```bash
cd …/docs/spawer-rs485/spawer_rs485_sniffer
pio run -t upload
```

V PIO Arduino / VS Code: **Open folder** → sniffer root → **Upload** (env `waveshare_esp32s3_lcd7b`).

1. USB-C do Waveshare, COM v Device Manageru.
2. Upload dokud PIO nehlásí success.
3. Po flashi **zavři a znovu otevři** monitor (Windows CDC po resetu často „ztratí“ port).
4. V boot logu ověř: `[UI] OK 1024x600`, `OUT=` / `IN=` / `SP=` / `H100=`, `H100_START` po Start.

USB flags: `ARDUINO_USB_CDC_ON_BOOT=1`, `ARDUINO_USB_MODE=1`, monitor **115200**.

## 4. USB monitor

```bash
pio device monitor -e waveshare_esp32s3_lcd7b
```

1. Ve Windows Device Manageru vyber COM desky („USB Serial Device“ / CDC).
2. **Jeden** monitor najednou.
3. Po **hardwarovém resetu** monitor **zavři a znovu otevři**.
4. Firmware čeká max ~3 s na CDC; každých **30 s** píše `alive`.
5. **PuTTY často nepřijímá vstup** — poll běží sám; `h100` / `sp+` / `start` přes `pio device monitor`.

## 5. Spawer — hlavní test (H100 Start + LCD)

```
Spawer RS485 A  ───  Waveshare RS485 A
Spawer RS485 B  ───  Waveshare RS485 B
GND (volitelně) ───  společná zem
```

1. Zapoj Spawer A/B (paralelní napojení na panel).
2. Dip **RS485 termination** na Waveshare: **OFF / NC**.
3. **Reflash** + otevři monitor @ 115200 (`pio device monitor`).
4. LCD: **OUT °C**, **IN °C**, **SP °C** (−/+), **H100**, **H101**, **H13**; Serial = `OUT=` / `IN=` / `SP=` / `H100=` + quiet DIFF CHANGE.
5. **LCD START** / `fc16` — FC16 (+retries) + confirm + **H13 force** + SCAN6 + DIFFAULT (**SCANFC ne auto**).
6. **LCD STOP** / `stop` / `fc16 0`.
7. **SP − / +** nebo `sp-` / `sp+` / `sp 200` — FC16 setpoint ±0.5 °C.
8. **Panel Start** — **`H100=… CHANGE` 1→0** → auto **SCAN6**; sleduj **`H13=… bits=`**.
9. Ručně: **`scanfc`**, **`scan6`**, **`diffault`**.

**Očekávání:** po Start **`H13=64 bits=6`** na Serial + LCD (hypotéza = panel error 6). LCD status ukáže `START OK` / `START FAIL`.

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
| *(auto fast)* | FC03 H100–101 + H0–5 (OUT/IN/SP) + 500–520 ~1 s |
| *(LCD Start / panel 1→0)* | po FC16 confirm nebo H100 **1→0** → **`SCAN6`** |
| `h100` / `start100` / `fc16` | LCD Start (**FC16**, retries) |
| `stop` / `fc16 0` | LCD Stop (**FC16 H100=0**, retries) |
| `sp+` / `sp-` / `sp <raw>` | setpoint FC16 ±0.5 °C / absolute raw×0.1 |
| `scan6` / `err6` | ruční FC03 SCAN6 raw=6 |
| `h100alt` | FC16 first, pak FC06 experiment |
| `fc06` | experiment FC06 (typicky **nic**) |
| `poll` / `p` | jeden FC03 temp/error scan |
| `quiet` / `q` | quiet Serial (default) |
| `verbose` / `v` | dump bloků hex |
| `start` / `s` | START_PROBE FC06 kandidáti 0–31 |
| `help` | nápověda |
| BOOT (fyzické) | **download / USB-reset** — **ne** runtime START |

## 8. Co znamenají výsledky

| Tag / výsledek | Význam |
|----------------|--------|
| `OUT=N` / `IN=N` / `SP=N` | FC03 mapped holdings (raw×0.1 °C); CHANGE on delta |
| `H100=N` / `H101=N` | FC03 holding 100 / 101 |
| `H100=N CHANGE` | holding 100 se změnil — **panel Start → typicky 1→0** |
| `H100_START tap` | LCD CLICKED (debounce OK) |
| `H100_START tap IGNORED` | busy/debounce — zápis už běží |
| `H100_START TX/RX` | quiet-bus **FC16** (+ retries) — hex + `result=ACK\|EXC6\|TIMEOUT` |
| `SP_SET` | FC16 setpoint write + confirm |
| `MODBUS_OK` | aktivní čtení/zápis OK |
| `MODBUS_TIMEOUT` | žádná odpověď — **A↔B**, napájení, baud |
| `H13=N bits=B` | DIFF status; **64 → bits=6** ≈ panel error 6 |
| `ERR6_ADDR=N` | SCAN6 hit — holding **N** má raw **6** |

**Verdikt Round 1:** aspoň jeden `MODBUS_OK` = Spawer mluví Modbus RTU.  
FC03 potvrzeno. **H100** = Start watch. OUT/IN/SP = best candidates (remapable).

## 9. Časté problémy

| Problém | Co zkusit |
|---------|-----------|
| Samé `MODBUS_TIMEOUT` | **Prohoď A↔B**, napájení Spaweru, `4800`/`19200` |
| Start/Stop až na N-tý pokus | Nový firmware: delší quiet + retries; sleduj LCD status |
| LCD černý / bez UI | Reflash; EXIO USB=0; jas přes IO ext |
| Touch double-fire | Debounce + in-flight lock (tap IGNORED) |
| FC04/01 se neposílají | očekávané — scan jen FC03 |
| BOOT „nereaguje“ na START | **normální na 7B** — Start = LCD / `h100` |
| Prázdný USB monitor | Znovu otevři COM po resetu; CDC flags |
| OUT/IN/SP nesedí s panelem | Remap `MB_*_ADDR` v `main.cpp` |

---

### Soubory

```
(repo root = sniffer PIO project)
├── README.md
├── hardware-pinout.md
├── platformio.ini
├── include/          ← lv_conf.h, board_7b.h, ui_test.h
└── src/
    ├── main.cpp      ← Modbus + remap constants OUT/IN/SP
    ├── ui_test.cpp   ← LVGL IN/OUT/SP± + START/STOP
    └── board/        ← Waveshare RGB/GT911
```

### Mimo Round 1

Plné řídicí UI, MQTT/HA, ověřené power/mode registry mimo H100 experiment.
