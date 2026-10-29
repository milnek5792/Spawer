# Spawer RS485 — návod (Round 1)

## 1. Co to dělá

Firmware na **Waveshare ESP32-S3-Touch-LCD-7B** (UART **RX=GPIO15**, **TX=GPIO16**, auto DE/RE):

### Aktuální cíl: H100 Start (panel 1→0) + LCD test UI

1. **Quiet Serial (default)** — jen **`H100=` / `H1=` / `H101=`** (+ **`CHANGE`**), **`H100 busy/ex6`** (rate-limited), **`H100_START`**, `alive` / `SILENT`. Žádné TEMP/ERROR / bulk dump / hex.
2. **FC03 fast watch** — slave **1**, **9600 8N1**, každých **~1 s** (priorita **H100**):
   - **holding 100–101** (blok qty 2) → **`H100=N`** / **`H101=N`** (při OK + LCD)
   - **Modbus EXC 0x06** (Slave Device Busy): **ne** mazat last good / UI „—“; **2–3× retry** + backoff ~200 ms; log **`H100 busy/ex6`**
   - **≠** `ERROR_ADDR holding=… raw=6` (holding error kód — jen `verbose`)
   - settle, pak **holding 1** → **`H1=N`**; na LCD **°C = raw × 0.1**
   - **H0** volitelně **OFF**; auto temp/error scan **OFF** (nehladoví H100)
   - **Milan:** panel **Start** → **`H100=… CHANGE`** (**1→0**)
3. **LCD test UI** (LVGL): **H1 (°C)**, **H100**, **H101** + **START** → **quiet-bus + FC16 qty1 holding 100** (toggle: **0** když H100==1, jinak **1**) + settle ≥800 ms. Serial: `h100` / `fc16` (= totéž). **FC06 na této pumpě nic nedělá.** Alts: `h100alt`.
4. **Temp/error scan** — jen ručně `poll` (+ `verbose` pro TEMP/ERROR / hex). **FC04/01/02 OFF.**
5. **START_PROBE** — auto OFF; jen `start` / `s`.

Hledej: **`H100=` / `CHANGE`**, **`H100_START tap`**, **`H100_START` TX/RX hex**, **`H1=`**, **`H101=`**, případně **`H100 busy/ex6`**. (`verbose` → TEMP/ERROR.)

> **Milan — REFLASH** (Start = **FC16 only**; FC06 nic nedělá):
> ```bash
> cd …/docs/spawer-rs485/spawer_rs485_sniffer
> pio run -t upload
> pio device monitor -e waveshare_esp32s3_lcd7b
> ```
> Po uploadu zavři/znovu otevři monitor (Windows CDC). Serial = `H100=` / `H1=` / `H101=` ~1 s; při busy jen občas `H100 busy/ex6` (UI drží last).  
> **Neni prepínac** — napiš: `fc16` nebo `h100alt`. **Panel Start** → `H100=… CHANGE` (1→0). **LCD START** / `h100` / `fc16` → quiet-bus **FC16** qty1 + TX/RX (`ACK` / `EXC6` / `TIMEOUT`).

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
4. LCD ukáže **H1 °C**, **H100**, **H101**; Serial quiet = `H100=` / `H1=` / `H101=` ~1 s + light **`H500–520`** (CHANGE/nonzero).
5. **Panel Start** — hledej **`H100=… CHANGE`** (**1→0**) a hned **`=== ERR_DUMP (H100 1->0) ===`** → **`Hnnn=`** / **`ERR_ADDR=nnn raw=…`** (jen nonzero; 500–506 nestačilo).
6. **LCD START** (nebo Serial `h100` / `fc16`) — **FC16 only**:
   - `H100_START tap (UI CLICKED)` — touch dorazil
   - `=== H100_START (UI) ===` + `quiet-bus` + **`H100_START TX … FC16 …`** hex + **`H100_START RX …`** / `TIMEOUT`
   - `H100_START result=ACK|EXC6|TIMEOUT|… (FC16 qty1)`
   - Po confirm: **`=== ERR_DUMP (post-FC16 Start) ===`** (kandidáti + sparse) → `Hnnn=` / `ERR_ADDR=`
   - Bez **tap** řádku = touch path; tap + TX ale silence/EXC = Modbus / busy bus
7. Ručně **`errdump`** — stejný one-shot lov (když panel Start bez FC16).
8. Občas **`H100 busy/ex6`** = Modbus Slave Busy — UI **ne** „—“; ≠ `ERR_ADDR` holding raw.
9. **ERR_DUMP okna:** 0–31, 100–120, 200–220, 400–420, 500–520, 1000–1020, 2000–2020 + sparse 50/150/250/300/350/600/700/800/900/1500/2500/3000.
10. Pošli řádky `H100=` / `ERR_DUMP` / `Hnnn=` / `ERR_ADDR=` / `CHANGE` / `H100_START` TX|RX / `result=`.

**Očekávání:** H100 Start dál funguje; po Start/`1→0` uvidíš **ERR_DUMP** (nejen 500–506); FC06 nic nedělá.

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
| *(LCD Start / panel 1→0)* | po FC16 confirm nebo H100 **1→0** → **`ERR_DUMP`** (`Hnnn=` / `ERR_ADDR=`) |
| `h100` / `start100` / `fc16` | totéž jako LCD Start (**FC16 only**; bez tap řádku) + ERR_DUMP |
| `fc16 0` / `fc16 1` | FC16 qty1 s explicitní hodnotou |
| `errdump` / `err` | ruční one-shot ERR_DUMP (kandidáti + sparse) |
| `h100alt` | FC16 first, pak FC06 experiment — log `ACK` / `EXC6` / `TIMEOUT` |
| `fc06` | experiment FC06 (na této pumpě typicky **nic**) |
| *(auto scan)* | OFF — ručně `poll` → `TEMP_ADDR` / `ERROR_ADDR` |
| `poll` / `p` | jeden FC03 temp/error scan hned |
| `quiet` / `q` | jen `H1/H100/H101` + busy/ex6 + H100_START (default) |
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
| `ERROR_ADDR holding=N raw=6` | holding **10..20** má raw **6** (kandidát error kódu) |
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
