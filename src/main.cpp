/*
 * Spawer Round 1 — RS485 sniffer + aktivní Modbus RTU master + LCD test UI
 * Deska: Waveshare ESP32-S3-Touch-LCD-7B
 * UART RS485: RX=GPIO15, TX=GPIO16 (auto DE/RE)
 * USB log: 115200 (Serial zůstává)
 *
 * LCD (LVGL): H1 jako °C (raw×0.1), H100, H101 + velké Start
 *   → quiet-bus + FC16 qty1 holding 100 (toggle: 0 když H100==1, jinak 1)
 *   Milan: FC06 na této pumpě nic nedělá — Start = FC16 only
 *   Milan: panel Start → H100 CHANGE 1→0; LCD Start musí ukázat H100_START + TX/RX
 *
 * Default (Spawer mód) — quiet Serial + fast H100/H1/H101:
 *   - Serial quiet: jen H1= / H100= / H101= [/CHANGE], H100_START (+ TX/RX hex),
 *     H100 busy/ex6 (rate-limited), alive/SILENT — NE flood „fail“
 *   - Modbus EXC 0x06 (Slave Device Busy) ≠ ERROR_ADDR holding raw=6
 *   - EXC6 / TO: keep last good haveH100 (UI neznámé jen když ještě nikdy OK);
 *     2–3 retry s backoff 100–300 ms; stagger H100 ↔ H1
 *   - verbose: dump bloků + TEMP_ADDR / ERROR_ADDR + pasivní hex frames
 *   - aktivní Modbus poll (slave 1, 9600 8N1):
 *       fast ~1.0 s (priorita H100):
 *         FC03 holding 100–101 (qty 2) → H100= / H101=
 *         settle, pak FC03 holding 1 → H1=
 *         settle, FC03 holding 500–520 (quiet CHANGE/nonzero) — H500–506 nestačilo
 *         H0 volitelně OFF (MB_H0_POLL_ENABLED)
 *       ERROR dump (quiet, jen nonzero / CHANGE / ERR_ADDR):
 *         kandidáti 0–31, 100–120, 200–220, 400–420, 500–520, 1000–1020, 2000–2020
 *         + sparse 50/150/250/300/350/600/700/800/900/1500/2500/3000 (qty 8)
 *         trigger: po FC16 Start, při H100 1→0, Serial `errdump`
 *       lehký temp/error scan DEFAULT OFF (ručně `poll`; nehladoví H100)
 *       FC04 / FC01 / FC02 SKIP
 *   - START_PROBE FC06: auto OFF; ručně `start` (holding 0–31 kandidáti)
 *   - UI Start / h100 / fc16: pause poll → quiet bus ≥80 ms → FC16 qty1 → echo → settle
 *   - Po FC16: dual confirm H100/H101/H1 + jednou ERR dump (fault latch)
 *   - FC06 na této Spawer pumpě nic nedělá — primary Start = FC16 only
 *   - h100alt: experimentální sekvence (FC16 first; FC06 jen pro srovnání)
 *   - fc16 0|1 / h100fc16: explicitní hodnota; bez arg = toggle jako Start
 *   - fc06: experimentální one-shot FC06 (na této pumpě typicky nic)
 *   - Neni prepínac — napis: fc16   nebo  h100alt
 *   - BOOT 7B = download; GPIO0 = LCD G3; auto echo OFF
 *
 * Baud / HW test (Serial 115200):
 *   help | listen | b | baud | poll | nopol | start | h100 | fc16 | h100alt | fc06 | errdump
 *   verbose | quiet | echoon | echooff | 4800 | 9600 | … | echo | e | ping | tx | loop
 */

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "ui_test.h"

static const int RS485_RX_PIN = 15;
static const int RS485_TX_PIN = 16;
// Waveshare ESP32-S3-Touch-LCD-7B: GPIO0 = LCD G3 (RGB), ne volné user GPIO.
// Fyzické BOOT = download mode (držet při napájení) / USB flash — NE runtime START.
static const int BOOT_BTN_PIN = 0;
static const bool BOOT_BTN_RUNTIME_START = false;
static const uint32_t DEFAULT_BUS_BAUD = 9600;
static const uint32_t USB_BAUD = 115200;

static const uint32_t SILENT_MS = 10000;
static const uint32_t CDC_WAIT_MS = 3000;
static const uint32_t ALIVE_MS = 30000;
static const uint32_t ECHO_WAIT_MS = 50;
static const uint32_t AUTO_ECHO_INTERVAL_MS = 10000;
static const uint8_t AUTO_ECHO_COUNT = 3;  // only if autoEchoEnabled
// Auto START_PROBE vypnuto při FC03 temp scanu (agresivní FC06 by rušil mapování).
static const uint32_t AUTO_START_PROBE_MS = 5000;
static const bool AUTO_START_PROBE_ENABLED = false;

// Modbus RTU master — fast H100/H1/H101; light temp/error scan OFF (nehladoví H100)
static const uint8_t MB_SLAVE = 1;
static const bool MB_H0_POLL_ENABLED = false;       // volitelně H0 — default OFF
static const bool MB_LIGHT_SCAN_ENABLED = false;    // auto TEMP/ERROR scan OFF (ručně `poll`)
static const uint32_t MB_FAST_POLL_MS = 1000;       // H100-101 + H1 ~1 s (méně hammer při busy)
static const uint32_t MB_FAST_INTER_MS = 200;       // mezera H100 blok ↔ H1 (bus settle)
static const uint32_t MB_POLL_INTERVAL_MS = 120000; // kdyby scan ON — velmi pomalu
static const uint32_t MB_RESPONSE_TIMEOUT_MS = 600;
static const uint32_t MB_INTER_REQ_MS = 300;  // turnaround 200–500 ms + RX wait
static const uint32_t MB_START_SETTLE_MS = 300;
static const uint32_t MB_H100_WRITE_SETTLE_MS = 800; // legacy settle (alt methods)
static const uint32_t MB_H100_CONFIRM_EARLY_MS = 250; // 1. FC03 po zápisu (chytit val=1 před clear)
static const uint32_t MB_H100_CONFIRM_LATE_MS = 500;  // 2. sample — auto-clear 1→0
static const uint32_t MB_H100_POLL_RESUME_MS = 300;   // po confirm hned znovu fast poll
static const uint32_t MB_QUIET_BUS_MS = 80;          // ≥50–100 ms silent před zápisem (3.5T + margin)
static const uint32_t MB_QUIET_BUS_MAX_MS = 500;     // max čekání na ticho (panel může mluvit)
static const uint32_t MB_BUSY_BACKOFF_MS = 200;      // EXC 0x06 / TO retry backoff (100–300)
static const uint8_t MB_BUSY_RETRIES = 3;            // pokusy na cyklus (1 + 2 retry)
static const uint8_t MB_EXC_SLAVE_BUSY = 0x06;       // Modbus exception: Slave Device Busy
static const uint16_t MB_START_WRITE_VALUE = 1;
static const uint16_t MB_H100_START_ADDR = 100;      // 0-based holding 100 (ne 40001)
static const uint16_t MB_H100_START_VALUE = 1;       // panel Start → H100 CHANGE 1→0
static const size_t MAX_FRAME = 256;
static const size_t PROP_LEN_MIN = 70;
static const size_t PROP_LEN_MAX = 90;
static const uint16_t MB_HOLD_QTY = 32;   // START_PROBE dump 0–31
static const uint16_t MB_SCAN_BLOCK = 16; // FC03 bloky po 16
static const uint16_t MB_ERROR_START = 10;  // legacy verbose scan: holding 10–20
static const uint16_t MB_ERROR_QTY = 22;    // jeden blok 10–31 (verbose only)
static const uint16_t ERROR_HOLD_MIN = 10;
static const uint16_t ERROR_HOLD_MAX = 20;
static const uint16_t ERROR_RAW_MATCH = 6;
// Quiet continuous: expand 500–506 → 500–520 (Milan: 500–506 empty)
static const uint16_t MB_H500_ADDR = 500;
static const uint16_t MB_H500_QTY = 21;  // 500..520 inclusive
static const uint16_t MB_ERR_DUMP_MAX_QTY = 32;
static const uint32_t MB_ERR_DUMP_COOLDOWN_MS = 5000;  // panel 1→0 auto dump
static const uint32_t MB_ERR_WIN_GAP_MS = 80;          // mezera mezi ERR okny (latch)
static const uint16_t CANDIDATE_TEMP_MIN = 150;
static const uint16_t CANDIDATE_TEMP_MAX = 350;
static const size_t MAX_TEMP_CANDS = 64;
static const size_t MAX_ERROR_CANDS = 16;

struct ErrWin {
  uint16_t start;
  uint16_t qty;
  const char *label;
};

// Candidate error windows (H500–506 empty → hunt elsewhere). 10–20 ⊂ 0–31.
static const ErrWin MB_ERR_WINS[] = {
    {0, 32, "0-31"},
    {100, 21, "100-120"},
    {200, 21, "200-220"},
    {400, 21, "400-420"},
    {500, 21, "500-520"},
    {1000, 21, "1000-1020"},
    {2000, 21, "2000-2020"},
};
static const size_t MB_ERR_WINS_N = sizeof(MB_ERR_WINS) / sizeof(MB_ERR_WINS[0]);

// One-shot wider sparse (short blocks — mimo husté kandidáty)
static const ErrWin MB_ERR_SPARSE[] = {
    {50, 8, "50-57"},     {150, 8, "150-157"}, {250, 8, "250-257"},
    {300, 8, "300-307"},  {350, 8, "350-357"}, {600, 8, "600-607"},
    {700, 8, "700-707"},  {800, 8, "800-807"}, {900, 8, "900-907"},
    {1500, 8, "1500-1507"}, {2500, 8, "2500-2507"}, {3000, 8, "3000-3007"},
};
static const size_t MB_ERR_SPARSE_N = sizeof(MB_ERR_SPARSE) / sizeof(MB_ERR_SPARSE[0]);

static const uint8_t ECHO_PATTERN[] = {
    0xE5, 0x53, 0x50, 0x41, 0x57, 0x45, 0x52, 0x0A};
static const size_t ECHO_PATTERN_LEN = sizeof(ECHO_PATTERN);

static const uint32_t BAUD_TABLE[] = {4800, 9600, 19200, 38400, 57600, 115200};
static const size_t BAUD_TABLE_N = sizeof(BAUD_TABLE) / sizeof(BAUD_TABLE[0]);

enum class FrameClass : uint8_t {
  MODBUS,
  PROPRIETARY_80B,
  OTHER
};

struct MbReadReq {
  uint8_t fc;
  uint16_t start;
  uint16_t qty;
  const char *label;
};

// Primární scan: error blok 10–31 + temp kolem bází 0 / 500 / 2000 (bloky po 16).
// FC04/FC01/FC02 záměrně vynecháno (timeout šum).
static const MbReadReq MB_REQS[] = {
    // error kód: holding 10–20 (čteme 10–31 jako jeden blok)
    {0x03, MB_ERROR_START, MB_ERROR_QTY, "FC03 holding 10-31"},
    // báze 0 → 0–63 (temp scan; 0–15/16–31 překryjí 10–20 — OK)
    {0x03, 0, MB_SCAN_BLOCK, "FC03 holding 0-15"},
    {0x03, 16, MB_SCAN_BLOCK, "FC03 holding 16-31"},
    {0x03, 32, MB_SCAN_BLOCK, "FC03 holding 32-47"},
    {0x03, 48, MB_SCAN_BLOCK, "FC03 holding 48-63"},
    // báze 500 → 480–543
    {0x03, 480, MB_SCAN_BLOCK, "FC03 holding 480-495"},
    {0x03, 496, MB_SCAN_BLOCK, "FC03 holding 496-511"},
    {0x03, 512, MB_SCAN_BLOCK, "FC03 holding 512-527"},
    {0x03, 528, MB_SCAN_BLOCK, "FC03 holding 528-543"},
    // báze 2000 → 1984–2047
    {0x03, 1984, MB_SCAN_BLOCK, "FC03 holding 1984-1999"},
    {0x03, 2000, MB_SCAN_BLOCK, "FC03 holding 2000-2015"},
    {0x03, 2016, MB_SCAN_BLOCK, "FC03 holding 2016-2031"},
    {0x03, 2032, MB_SCAN_BLOCK, "FC03 holding 2032-2047"},
};
static const size_t MB_REQS_N = sizeof(MB_REQS) / sizeof(MB_REQS[0]);

// Fairland: power = coil0 (u Spawer timeout). Holding[0] live ~200 = spíš režim.
// Kandidáti: adresy, které bývají 0 a mohou být command/power v jiné mapě.
static const uint16_t MB_START_CANDIDATES[] = {4, 5, 6, 9, 16, 17, 18, 19};
static const size_t MB_START_CANDIDATES_N =
    sizeof(MB_START_CANDIDATES) / sizeof(MB_START_CANDIDATES[0]);

static const MbReadReq MB_H0_REQ = {0x03, 0, 1, "FC03 holding 0"};
static const MbReadReq MB_H1_REQ = {0x03, 1, 1, "FC03 holding 1"};
static const MbReadReq MB_H100_BLOCK = {0x03, 100, 2, "FC03 holding 100-101"};
static const MbReadReq MB_H500_BLOCK = {0x03, MB_H500_ADDR, MB_H500_QTY,
                                        "FC03 holding 500-520"};
static const MbReadReq MB_START_BEFORE = {0x03, 0, MB_HOLD_QTY,
                                          "FC03 holding 0-31 (pred)"};
static const MbReadReq MB_START_AFTER = {0x03, 0, MB_HOLD_QTY,
                                         "FC03 holding 0-31 (po)"};

static uint32_t busBaud = DEFAULT_BUS_BAUD;
static uint8_t buf[MAX_FRAME];
static size_t buflen = 0;
static uint32_t lastByteUs = 0;
static uint32_t lastActivityMs = 0;
static bool silentAnnounced = true;
static uint32_t counts[4] = {0, 0, 0, 0};  // modbus, prop, other, silent_events
static char cmdBuf[48];
static size_t cmdLen = 0;
static uint32_t lastAliveMs = 0;

// Auto echo — default OFF ve Spawer módu (ruší bus)
static bool autoEchoEnabled = false;
static uint32_t autoEchoStartMs = 0;
static uint8_t autoEchoDone = 0;
static bool bootBtnPrev = true;

// Modbus poll — default ON (Spawer mód); fast H1/H100/H101; temp/error lehce
static bool mbPollEnabled = true;
static bool mbVerbose = false;
static bool mbScanBusy = false;
static uint32_t mbNextFastMs = 0;
static uint32_t mbNextPollMs = 0;
static uint32_t mbCycleCount = 0;
static uint32_t mbOkCount = 0;
static uint32_t mbTimeoutCount = 0;
static uint16_t lastH0 = 0;
static uint16_t lastH1 = 0;
static uint16_t lastH100 = 0;
static uint16_t lastH101 = 0;
static uint16_t lastH500[MB_H500_QTY] = {0};
static bool haveH0 = false;
static bool haveH1 = false;
static bool haveH100 = false;
static bool haveH101 = false;
static bool haveH500 = false;
static bool mbReqSkip[MB_REQS_N] = {false};
static bool mbErrDumpBusy = false;
static uint32_t mbLastErrDumpMs = 0;
// Rate-limit quiet busy/TO logs (ne flood „H100 fail“)
static uint32_t mbLastH100BusyLogMs = 0;
static const uint32_t MB_BUSY_LOG_MIN_MS = 5000;

// Výsledek quiet FC03 read — odliší Modbus EXC 0x06 od timeoutu / rámce.
enum class MbQuietStatus : uint8_t {
  Ok = 0,
  Timeout,
  CrcErr,
  FrameErr,
  ExcBusy,   // Modbus exception 0x06 Slave Device Busy
  ExcOther   // jiná Modbus exception (ne holding ERROR raw=6)
};

struct TempCand {
  uint16_t addr;
  uint16_t raw;
};
static TempCand scanCands[MAX_TEMP_CANDS];
static size_t scanCandN = 0;
static uint16_t prevCandAddrs[MAX_TEMP_CANDS];
static size_t prevCandN = 0;
static bool havePrevScan = false;

struct ErrorCand {
  uint16_t addr;
  uint16_t raw;
};
static ErrorCand errorCands[MAX_ERROR_CANDS];
static size_t errorCandN = 0;

// Auto START_PROBE ~5 s po bootu (BOOT GPIO0 na 7B nefunguje jako user button)
static bool autoStartProbeDone = false;
static uint32_t autoStartProbeAtMs = 0;

static uint32_t frameGapUs() {
  uint32_t us = (uint32_t)((11.0f * 3.5f * 1000000.0f) / (float)busBaud);
  if (us < 1500) us = 1500;
  return us;
}

static uint16_t modbusCrc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

static bool looksModbus(const uint8_t *d, size_t n) {
  if (n < 4 || n > 256) return false;
  uint16_t got = (uint16_t)d[n - 2] | ((uint16_t)d[n - 1] << 8);
  uint16_t calc = modbusCrc16(d, n - 2);
  if (got != calc) return false;

  uint8_t fc = d[1];
  if (fc >= 0x01 && fc <= 0x06) return true;
  if (fc == 0x0F || fc == 0x10 || fc == 0x17) return true;
  if (fc >= 0x81 && fc <= 0x86) return true;
  if (fc == 0x8F || fc == 0x90 || fc == 0x97) return true;
  return n >= 5;
}

static bool looksProprietary80(const uint8_t *d, size_t n) {
  if (n < PROP_LEN_MIN || n > PROP_LEN_MAX) return false;
  uint8_t h = d[0];
  return (h == 0xDD || h == 0xD2 || h == 0xCC || h == 0xCD);
}

static const char *className(FrameClass c) {
  switch (c) {
    case FrameClass::MODBUS: return "MODBUS";
    case FrameClass::PROPRIETARY_80B: return "PROPRIETARY_80B";
    default: return "OTHER";
  }
}

static void hexDump(const uint8_t *d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (i && (i % 16) == 0) Serial.print("\n           ");
    if (d[i] < 16) Serial.print('0');
    Serial.print(d[i], HEX);
    Serial.print(' ');
  }
}

static void emitFrame(const uint8_t *d, size_t n) {
  FrameClass c = FrameClass::OTHER;
  if (looksModbus(d, n)) {
    c = FrameClass::MODBUS;
    counts[0]++;
  } else if (looksProprietary80(d, n)) {
    c = FrameClass::PROPRIETARY_80B;
    counts[1]++;
  } else {
    counts[2]++;
  }

  // Quiet default: no passive hex / unused-register dumps
  if (!mbVerbose) return;

  Serial.printf("[%lu ms] %s len=%u | ", (unsigned long)millis(), className(c),
                (unsigned)n);
  hexDump(d, n);
  Serial.println();
  Serial.printf("  totals: MODBUS=%lu PROP80=%lu OTHER=%lu SILENT_ev=%lu\n",
                (unsigned long)counts[0], (unsigned long)counts[1],
                (unsigned long)counts[2], (unsigned long)counts[3]);
}

static void flushRs485Rx() {
  while (Serial1.available()) {
    (void)Serial1.read();
  }
  buflen = 0;
}

static void printTxPattern() {
  Serial.print(F("TX ("));
  Serial.print((unsigned)ECHO_PATTERN_LEN);
  Serial.print(F(" B): "));
  hexDump(ECHO_PATTERN, ECHO_PATTERN_LEN);
  Serial.println();
}

static void runEchoTest(bool waitForRx) {
  flushRs485Rx();
  delay(2);
  flushRs485Rx();

  Serial.println(F("--- RS485 echo / TX test ---"));
  Serial.printf("Bus UART: RX=GPIO%d TX=GPIO%d @ %lu 8N1 (auto DE/RE)\n",
                RS485_RX_PIN, RS485_TX_PIN, (unsigned long)busBaud);
  printTxPattern();
  Serial.println(F("Note: shorting A to B is NOT RS485 loopback."));
  Serial.println(F("Waveshare SP3485 auto-DE usually has NO local echo (expected)."));
  Serial.println(F("POZOR: na zivem Spawer bus echo nerus — jen vedome."));

  size_t written = Serial1.write(ECHO_PATTERN, ECHO_PATTERN_LEN);
  Serial1.flush();
  Serial.printf("Wrote %u / %u bytes on Serial1\n", (unsigned)written,
                (unsigned)ECHO_PATTERN_LEN);

  if (!waitForRx) {
    Serial.println(F("tx done — if USB-RS485 dongle is on A/B, you should see these bytes there."));
    Serial.println(F("Back to listen mode."));
    lastActivityMs = millis();
    silentAnnounced = false;
    return;
  }

  delay(ECHO_WAIT_MS);

  uint8_t rx[MAX_FRAME];
  size_t rxn = 0;
  while (Serial1.available() && rxn < sizeof(rx)) {
    int b = Serial1.read();
    if (b < 0) break;
    rx[rxn++] = (uint8_t)b;
  }

  Serial.printf("RX after %lu ms: %u bytes\n", (unsigned long)ECHO_WAIT_MS,
                (unsigned)rxn);
  Serial.print(F("RX hex: "));
  if (rxn == 0) {
    Serial.println(F("(none)"));
  } else {
    hexDump(rx, rxn);
    Serial.println();
  }

  if (rxn == 0) {
    Serial.println(F("Result: NO_LOCAL_ECHO (expected on half-duplex auto-DE)."));
    Serial.println(F("To verify TX: wire A/B to USB-RS485 dongle and watch PC terminal,"));
    Serial.println(F("or attach a peer that transmits and watch this sniffer (listen)."));
  } else {
    bool match = (rxn == ECHO_PATTERN_LEN) &&
                 (memcmp(rx, ECHO_PATTERN, ECHO_PATTERN_LEN) == 0);
    if (match) {
      Serial.println(F("Result: LOCAL_ECHO (transceiver or wiring fed TX back to RX)."));
    } else {
      Serial.println(F("Result: LOCAL_ECHO / bus activity (RX != TX pattern — dump above)."));
    }
  }
  Serial.println(F("Back to listen mode."));
  lastActivityMs = millis();
  silentAnnounced = false;
}

// --- Modbus RTU master -----------------------------------------------------

static size_t mbBuildReadReq(uint8_t *out, size_t outCap, const MbReadReq &r) {
  if (outCap < 8) return 0;
  out[0] = MB_SLAVE;
  out[1] = r.fc;
  out[2] = (uint8_t)(r.start >> 8);
  out[3] = (uint8_t)(r.start & 0xFF);
  out[4] = (uint8_t)(r.qty >> 8);
  out[5] = (uint8_t)(r.qty & 0xFF);
  uint16_t crc = modbusCrc16(out, 6);
  out[6] = (uint8_t)(crc & 0xFF);         // CRC low first (Modbus RTU)
  out[7] = (uint8_t)(crc >> 8);
  return 8;
}

// FC06 Write Single Register — addr/value big-endian; CRC lo then hi (RTU)
static size_t mbBuildWriteSingle(uint8_t *out, size_t outCap, uint16_t addr,
                                 uint16_t value) {
  if (outCap < 8) return 0;
  out[0] = MB_SLAVE;
  out[1] = 0x06;
  out[2] = (uint8_t)(addr >> 8);
  out[3] = (uint8_t)(addr & 0xFF);
  out[4] = (uint8_t)(value >> 8);
  out[5] = (uint8_t)(value & 0xFF);
  uint16_t crc = modbusCrc16(out, 6);
  out[6] = (uint8_t)(crc & 0xFF);
  out[7] = (uint8_t)(crc >> 8);
  return 8;
}

// FC16 (0x10) Write Multiple Registers — qty=1 at addr (alternate Start method)
static size_t mbBuildWriteMultiple(uint8_t *out, size_t outCap, uint16_t addr,
                                   uint16_t qty, const uint16_t *vals) {
  if (!vals || qty == 0) return 0;
  size_t need = 9 + (size_t)qty * 2;  // + CRC(2) already in 9? → 7+bc+2
  // PDU: slave FC addr(2) qty(2) bc vals… crc(2) = 9 + qty*2
  need = 9 + (size_t)qty * 2;
  if (outCap < need) return 0;
  out[0] = MB_SLAVE;
  out[1] = 0x10;
  out[2] = (uint8_t)(addr >> 8);
  out[3] = (uint8_t)(addr & 0xFF);
  out[4] = (uint8_t)(qty >> 8);
  out[5] = (uint8_t)(qty & 0xFF);
  out[6] = (uint8_t)(qty * 2);
  for (uint16_t i = 0; i < qty; i++) {
    out[7 + i * 2] = (uint8_t)(vals[i] >> 8);
    out[8 + i * 2] = (uint8_t)(vals[i] & 0xFF);
  }
  size_t body = 7 + (size_t)qty * 2;
  uint16_t crc = modbusCrc16(out, body);
  out[body] = (uint8_t)(crc & 0xFF);
  out[body + 1] = (uint8_t)(crc >> 8);
  return body + 2;
}

// Pause until RX silent for quietMs (Modbus 3.5 char + margin). Returns false if noisy.
static bool waitBusQuiet(uint32_t quietMs, uint32_t maxWaitMs) {
  flushRs485Rx();
  uint32_t t0 = millis();
  uint32_t lastActivity = t0;
  while ((millis() - t0) < maxWaitMs) {
    if (Serial1.available()) {
      flushRs485Rx();
      lastActivity = millis();
    } else if ((millis() - lastActivity) >= quietMs) {
      return true;
    }
    uiTestTick();
    delay(1);
  }
  bool ok = (millis() - lastActivity) >= quietMs;
  if (!ok) {
    Serial.printf("  QUIET_BUS incomplete (wanted %lu ms silent in %lu ms)\n",
                  (unsigned long)quietMs, (unsigned long)maxWaitMs);
  }
  return ok;
}

// Result tag for H100 write attempts (ACK / EXC / TO / …)
enum class MbWriteResult : uint8_t {
  Ack = 0,
  ExcBusy,
  ExcOther,
  Timeout,
  CrcErr,
  FrameErr,
  TxErr
};

static const char *mbWriteResultStr(MbWriteResult r) {
  switch (r) {
    case MbWriteResult::Ack: return "ACK";
    case MbWriteResult::ExcBusy: return "EXC6";
    case MbWriteResult::ExcOther: return "EXC";
    case MbWriteResult::Timeout: return "TIMEOUT";
    case MbWriteResult::CrcErr: return "CRC_ERR";
    case MbWriteResult::FrameErr: return "FRAME";
    case MbWriteResult::TxErr: return "TX_ERR";
    default: return "?";
  }
}

static size_t mbExpectedRespLen(const MbReadReq &r) {
  // Normal response: addr + fc + byte_count + data + crc(2)
  if (r.fc == 0x01 || r.fc == 0x02) {
    size_t bytes = (r.qty + 7) / 8;
    return 3 + bytes + 2;
  }
  // FC03 / FC04
  return 3 + (size_t)r.qty * 2 + 2;
}

// Keep LVGL/touch alive during Modbus waits (Start click needs lv_timer_handler)
static void delayPumpUi(uint32_t ms) {
  uint32_t t0 = millis();
  while ((millis() - t0) < ms) {
    uiTestTick();
    delay(5);
  }
}

static size_t mbReadResponse(uint8_t *rx, size_t rxCap, size_t expectMin,
                             uint32_t timeoutMs) {
  size_t rxn = 0;
  uint32_t t0 = millis();
  uint32_t lastRxMs = t0;
  uint32_t gapMs = (frameGapUs() / 1000UL) + 2UL;
  if (gapMs < 4) gapMs = 4;
  uint32_t lastUiMs = t0;

  while ((millis() - t0) < timeoutMs) {
    while (Serial1.available() && rxn < rxCap) {
      int b = Serial1.read();
      if (b < 0) break;
      rx[rxn++] = (uint8_t)b;
      lastRxMs = millis();
      lastActivityMs = lastRxMs;
      silentAnnounced = false;
    }
    // Settled frame (inter-char gap) — accept expected length or any ≥5 B PDU
    if (rxn > 0 && (millis() - lastRxMs) >= gapMs) {
      if (rxn >= expectMin || rxn >= 5) break;
    }
    // Pump UI ~every 10 ms so GT911 clicks are not starved by Modbus TO
    uint32_t now = millis();
    if ((now - lastUiMs) >= 10) {
      uiTestTick();
      lastUiMs = now;
    }
    delay(1);
  }
  return rxn;
}

static bool isCandidateTemp(uint16_t v) {
  return v >= CANDIDATE_TEMP_MIN && v <= CANDIDATE_TEMP_MAX;
}

static bool isErrorAddrHit(uint16_t addr, uint16_t v) {
  return addr >= ERROR_HOLD_MIN && addr <= ERROR_HOLD_MAX &&
         v == ERROR_RAW_MATCH;
}

static void scanCandReset() {
  scanCandN = 0;
  errorCandN = 0;
}

static void scanCandAdd(uint16_t addr, uint16_t raw) {
  for (size_t i = 0; i < scanCandN; i++) {
    if (scanCands[i].addr == addr) {
      scanCands[i].raw = raw;
      return;
    }
  }
  if (scanCandN >= MAX_TEMP_CANDS) return;
  scanCands[scanCandN].addr = addr;
  scanCands[scanCandN].raw = raw;
  scanCandN++;
}

static void errorCandAdd(uint16_t addr, uint16_t raw) {
  for (size_t i = 0; i < errorCandN; i++) {
    if (errorCands[i].addr == addr) {
      errorCands[i].raw = raw;
      return;
    }
  }
  if (errorCandN >= MAX_ERROR_CANDS) return;
  errorCands[errorCandN].addr = addr;
  errorCands[errorCandN].raw = raw;
  errorCandN++;
}

static bool addrInPrev(uint16_t addr) {
  for (size_t i = 0; i < prevCandN; i++) {
    if (prevCandAddrs[i] == addr) return true;
  }
  return false;
}

// abs Modbus holding = block start (r.start) + offset within block.
static void emitTempAddrHit(uint16_t absHolding, uint16_t raw) {
  // Always both fields — never raw-only (that was the bug).
  Serial.printf("TEMP_ADDR holding=%u raw=%u\n", (unsigned)absHolding,
                (unsigned)raw);
}

static void emitErrorAddrHit(uint16_t absHolding, uint16_t raw) {
  // Holding error code (např. raw=6) ≠ Modbus exception 0x06 (Slave Device Busy).
  Serial.printf(
      "ERROR_ADDR holding=%u raw=%u (holding error code, not Modbus EXC)\n",
      (unsigned)absHolding, (unsigned)raw);
}

static void collectHoldingCandidates(const MbReadReq &r, const uint8_t *rx,
                                     size_t rxn) {
  if (r.fc != 0x03 || rxn < 5) return;
  uint8_t bc = rx[2];
  for (uint8_t i = 0; i + 1 < bc && (3 + i + 1) < rxn - 2; i += 2) {
    uint16_t addr = (uint16_t)(r.start + (i / 2));  // block_base + offset
    uint16_t v = ((uint16_t)rx[3 + i] << 8) | rx[3 + i + 1];
    if (isCandidateTemp(v)) scanCandAdd(addr, v);
    if (isErrorAddrHit(addr, v)) errorCandAdd(addr, v);
  }
}

// TEMP_ADDR / ERROR_ADDR jen ve verbose (quiet = H1/H100/H101 only).
static void printTempAddrSummary() {
  if (mbVerbose) {
    if (scanCandN == 0) {
      Serial.println(F("TEMP_ADDR none"));
    } else if (havePrevScan) {
      for (size_t i = 0; i < scanCandN; i++) {
        if (addrInPrev(scanCands[i].addr))
          emitTempAddrHit(scanCands[i].addr, scanCands[i].raw);
      }
      for (size_t i = 0; i < scanCandN; i++) {
        if (!addrInPrev(scanCands[i].addr))
          emitTempAddrHit(scanCands[i].addr, scanCands[i].raw);
      }
    } else {
      for (size_t i = 0; i < scanCandN; i++) {
        emitTempAddrHit(scanCands[i].addr, scanCands[i].raw);
      }
    }
  }

  prevCandN = 0;
  for (size_t i = 0; i < scanCandN && prevCandN < MAX_TEMP_CANDS; i++) {
    prevCandAddrs[prevCandN++] = scanCands[i].addr;
  }
  havePrevScan = true;
}

static void printErrorAddrSummary() {
  if (!mbVerbose) return;
  for (size_t i = 0; i < errorCandN; i++) {
    emitErrorAddrHit(errorCands[i].addr, errorCands[i].raw);
  }
}

// dump=true → hex data + TEMP_ADDR per hit. collect=true → sběr TEMP+ERROR.
static void mbLogHoldingData(const MbReadReq &r, const uint8_t *rx, size_t rxn,
                             bool collect, bool dump) {
  if (collect) collectHoldingCandidates(r, rx, rxn);
  if (!dump) return;

  if ((r.fc == 0x03 || r.fc == 0x04) && rxn >= 5) {
    uint8_t bc = rx[2];
    uint16_t hitAddr[32];
    uint16_t hitRaw[32];
    size_t hitN = 0;

    Serial.print(F("  data:"));
    for (uint8_t i = 0; i + 1 < bc && (3 + i + 1) < rxn - 2; i += 2) {
      uint16_t addr = (uint16_t)(r.start + (i / 2));  // block_base + offset
      uint16_t v = ((uint16_t)rx[3 + i] << 8) | rx[3 + i + 1];
      Serial.printf(" [%u]=%u", (unsigned)addr, (unsigned)v);
      if (r.fc == 0x03 && isCandidateTemp(v) && hitN < 32) {
        hitAddr[hitN] = addr;
        hitRaw[hitN] = v;
        hitN++;
      }
    }
    Serial.println();

    // Same clear line as quiet — never raw-only / never hide holding addr.
    for (size_t c = 0; c < hitN; c++) {
      emitTempAddrHit(hitAddr[c], hitRaw[c]);
    }
  } else if ((r.fc == 0x01 || r.fc == 0x02) && rxn >= 4) {
    Serial.printf("  bit0=%u\n", (unsigned)(rx[3] & 0x01));
  }
}

// true = slave odpověděl s platným CRC (OK nebo EXC)
static bool mbOneRequest(const MbReadReq &r) {
  uint8_t tx[8];
  size_t txn = mbBuildReadReq(tx, sizeof(tx), r);
  if (txn == 0) return false;

  flushRs485Rx();
  delay(2);
  flushRs485Rx();

  if (mbVerbose) {
    Serial.printf("[%lu ms] MODBUS_TX slave=%u %s | ",
                  (unsigned long)millis(), (unsigned)MB_SLAVE, r.label);
    hexDump(tx, txn);
    Serial.println();
  }

  size_t written = Serial1.write(tx, txn);
  Serial1.flush();
  if (written != txn) {
    Serial.printf("  MODBUS_TX_ERR wrote %u/%u\n", (unsigned)written,
                  (unsigned)txn);
  }

  // Small turnaround for half-duplex auto-DE
  delay(2);

  size_t expectMin = mbExpectedRespLen(r);
  if (expectMin < 5) expectMin = 5;
  uint8_t rx[MAX_FRAME];
  size_t rxn = mbReadResponse(rx, sizeof(rx), expectMin, MB_RESPONSE_TIMEOUT_MS);

  if (rxn == 0) {
    mbTimeoutCount++;
    if (mbVerbose) {
      Serial.println(
          F("  MODBUS_TIMEOUT — zadna odpoved (A<->B? napajeni? slave=1? baud?)"));
    }
    return false;
  }

  if (mbVerbose) {
    Serial.print(F("  MODBUS_RX len="));
    Serial.print((unsigned)rxn);
    Serial.print(F(" | "));
    hexDump(rx, rxn);
    Serial.println();
  }

  if (rxn < 5) {
    mbTimeoutCount++;
    if (mbVerbose) Serial.println(F("  MODBUS_TIMEOUT — prilis kratka odpoved"));
    return false;
  }

  uint16_t got = (uint16_t)rx[rxn - 2] | ((uint16_t)rx[rxn - 1] << 8);
  uint16_t calc = modbusCrc16(rx, rxn - 2);
  if (got != calc) {
    if (mbVerbose) {
      Serial.printf("  MODBUS_CRC_ERR got=0x%04X calc=0x%04X\n", got, calc);
    }
    return false;
  }

  if (rx[0] != MB_SLAVE && mbVerbose) {
    Serial.printf("  MODBUS_ADDR_ERR odpoved slave=%u (cekame %u)\n",
                  (unsigned)rx[0], (unsigned)MB_SLAVE);
  }

  if (rx[1] & 0x80) {
    if (mbVerbose) {
      uint8_t code = rx[2];
      if (code == MB_EXC_SLAVE_BUSY) {
        Serial.printf(
            "  MODBUS_EXC FC=0x%02X code=6 Slave Device Busy "
            "(not ERROR_ADDR holding raw=6)\n",
            (unsigned)rx[1]);
      } else {
        Serial.printf("  MODBUS_EXC FC=0x%02X code=%u\n", (unsigned)rx[1],
                      (unsigned)code);
      }
    }
    // Slave answered — bus + Modbus stack alive
    mbOkCount++;
    return true;
  }

  if (rx[1] != r.fc) {
    if (mbVerbose) {
      Serial.printf("  MODBUS_FC_ERR odpoved FC=0x%02X (cekame 0x%02X)\n",
                    (unsigned)rx[1], (unsigned)r.fc);
    }
    return false;
  }

  mbOkCount++;
  if (mbVerbose) {
    Serial.printf("  MODBUS_OK %s (CRC OK, %u B)\n", r.label, (unsigned)rxn);
  }
  mbLogHoldingData(r, rx, rxn, /*collect=*/true, /*dump=*/mbVerbose);
  return true;
}

// FC03 quiet read (bez TEMP/ERROR collect). Naplní vals[0..qty-1].
// Exception frame (FC|0x80 + code) NIKDY neparsujeme jako holding data —
// zvlášť EXC 0x06 ≠ holding ERROR raw=6.
static MbQuietStatus mbReadHoldingsQuietStatus(const MbReadReq &r, uint16_t *vals,
                                               size_t valsCap, uint8_t *excOut) {
  if (excOut) *excOut = 0;
  if (vals && valsCap > 0) {
    for (size_t i = 0; i < valsCap; i++) vals[i] = 0;
  }

  uint8_t tx[8];
  size_t txn = mbBuildReadReq(tx, sizeof(tx), r);
  if (txn == 0) return MbQuietStatus::FrameErr;

  flushRs485Rx();
  delay(2);
  flushRs485Rx();

  if (mbVerbose) {
    Serial.printf("[%lu ms] MODBUS_TX slave=%u %s | ",
                  (unsigned long)millis(), (unsigned)MB_SLAVE, r.label);
    hexDump(tx, txn);
    Serial.println();
  }

  size_t written = Serial1.write(tx, txn);
  Serial1.flush();
  if (written != txn) {
    Serial.printf("  MODBUS_TX_ERR wrote %u/%u\n", (unsigned)written,
                  (unsigned)txn);
  }
  delay(2);

  // Accept short exception PDUs (≥5 B) even when normal expectMin is larger.
  size_t expectMin = mbExpectedRespLen(r);
  if (expectMin < 5) expectMin = 5;
  uint8_t rx[MAX_FRAME];
  size_t rxn = mbReadResponse(rx, sizeof(rx), expectMin, MB_RESPONSE_TIMEOUT_MS);

  if (rxn == 0) {
    mbTimeoutCount++;
    if (mbVerbose) {
      Serial.printf("  %s TIMEOUT — zadna odpoved\n", r.label);
    }
    return MbQuietStatus::Timeout;
  }

  if (mbVerbose) {
    Serial.print(F("  MODBUS_RX len="));
    Serial.print((unsigned)rxn);
    Serial.print(F(" | "));
    hexDump(rx, rxn);
    Serial.println();
  }

  if (rxn < 5) {
    mbTimeoutCount++;
    if (mbVerbose) {
      Serial.printf("  %s TIMEOUT — prilis kratka odpoved\n", r.label);
    }
    return MbQuietStatus::Timeout;
  }

  uint16_t got = (uint16_t)rx[rxn - 2] | ((uint16_t)rx[rxn - 1] << 8);
  uint16_t calc = modbusCrc16(rx, rxn - 2);
  if (got != calc) {
    if (mbVerbose) {
      Serial.printf("  %s CRC_ERR got=0x%04X calc=0x%04X\n", r.label, got, calc);
    }
    return MbQuietStatus::CrcErr;
  }

  // Exception PDU: addr + (FC|0x80) + code + CRC — not holding payload.
  if (rx[1] & 0x80) {
    uint8_t code = rx[2];
    if (excOut) *excOut = code;
    mbOkCount++;  // slave answered — bus alive
    if (code == MB_EXC_SLAVE_BUSY) {
      if (mbVerbose) {
        Serial.printf(
            "  %s MODBUS_EXC code=6 Slave Device Busy (not ERROR_ADDR raw=6)\n",
            r.label);
      }
      return MbQuietStatus::ExcBusy;
    }
    if (mbVerbose) {
      Serial.printf("  %s MODBUS_EXC FC=0x%02X code=%u\n", r.label,
                    (unsigned)rx[1], (unsigned)code);
    }
    return MbQuietStatus::ExcOther;
  }

  if (rx[1] != 0x03 || rx[2] < (uint8_t)(r.qty * 2) ||
      rxn < (size_t)(3 + r.qty * 2 + 2)) {
    if (mbVerbose) Serial.printf("  %s FC/len ERR\n", r.label);
    return MbQuietStatus::FrameErr;
  }

  if (vals && valsCap > 0) {
    uint8_t bc = rx[2];
    size_t n = 0;
    for (uint8_t i = 0; i + 1 < bc && (3 + i + 1) < rxn - 2 && n < valsCap;
         i += 2) {
      vals[n++] = ((uint16_t)rx[3 + i] << 8) | rx[3 + i + 1];
    }
    if (n < r.qty) {
      if (mbVerbose) Serial.printf("  %s short data n=%u\n", r.label, (unsigned)n);
      return MbQuietStatus::FrameErr;
    }
  }
  mbOkCount++;
  return MbQuietStatus::Ok;
}

static bool mbReadHoldingsQuiet(const MbReadReq &r, uint16_t *vals, size_t valsCap) {
  return mbReadHoldingsQuietStatus(r, vals, valsCap, nullptr) == MbQuietStatus::Ok;
}

// FC03 with EXC6/TO retries; never clears caller's have* — last good stays.
static MbQuietStatus mbReadHoldingsQuietRetry(const MbReadReq &r, uint16_t *vals,
                                              size_t valsCap, uint8_t *excOut) {
  MbQuietStatus st = MbQuietStatus::Timeout;
  uint8_t exc = 0;
  for (uint8_t attempt = 0; attempt < MB_BUSY_RETRIES; attempt++) {
    if (attempt > 0) {
      delayPumpUi(MB_BUSY_BACKOFF_MS);
    }
    st = mbReadHoldingsQuietStatus(r, vals, valsCap, &exc);
    if (excOut) *excOut = exc;
    if (st == MbQuietStatus::Ok) return st;
    // Retry only busy / timeout — CRC/frame/other EXC: stop early
    if (st != MbQuietStatus::ExcBusy && st != MbQuietStatus::Timeout) {
      return st;
    }
  }
  return st;
}

static void logH100BusyOnce(MbQuietStatus st, uint8_t exc) {
  uint32_t now = millis();
  if ((now - mbLastH100BusyLogMs) < MB_BUSY_LOG_MIN_MS && mbLastH100BusyLogMs != 0) {
    return;
  }
  mbLastH100BusyLogMs = now;
  if (st == MbQuietStatus::ExcBusy ||
      (st == MbQuietStatus::ExcOther && exc == MB_EXC_SLAVE_BUSY)) {
    Serial.println(F("H100 busy/ex6"));
  } else if (st == MbQuietStatus::Timeout) {
    Serial.println(F("H100 busy/TO"));
  } else if (st == MbQuietStatus::ExcOther) {
    Serial.printf("H100 MODBUS_EXC code=%u\n", (unsigned)exc);
  } else {
    Serial.println(F("H100 busy/frame"));
  }
}

// Always print Hn= (quiet poll + post-write confirm). CHANGE when value moved.
static void emitHoldingWatch(const char *tag, uint16_t v, uint16_t *last,
                             bool *have) {
  if (*have && v != *last) {
    Serial.printf("%s=%u CHANGE\n", tag, (unsigned)v);
  } else {
    Serial.printf("%s=%u\n", tag, (unsigned)v);
  }
  *last = v;
  *have = true;
}

// Force-print after write even when value == last / == written (Milan confirm).
static void emitHoldingWatchForce(const char *tag, uint16_t v, uint16_t *last,
                                  bool *have, bool matchExpectedWrite) {
  if (*have && v != *last) {
    Serial.printf("%s=%u CHANGE\n", tag, (unsigned)v);
  } else if (matchExpectedWrite) {
    Serial.printf("%s=%u (confirm)\n", tag, (unsigned)v);
  } else {
    Serial.printf("%s=%u\n", tag, (unsigned)v);
  }
  *last = v;
  *have = true;
}

// Likely fault latch (Fairland/Poolstar-like): raw 1..99 or classic match=6.
static bool looksFaultCode(uint16_t v) {
  if (v == 0) return false;
  if (v == ERROR_RAW_MATCH) return true;
  if (v >= 1 && v <= 99) return true;
  if ((uint16_t)(v & 0xFF) == ERROR_RAW_MATCH && v <= 0xFF) return true;
  return false;
}

// Quiet emit one holding: Hnnn= [/CHANGE]; ERR_ADDR= on fault pattern / nonzero dump.
static void emitErrHoldingQuiet(uint16_t addr, uint16_t v, bool changed,
                                bool printZeros) {
  if (v == 0 && !changed && !printZeros) return;
  if (changed) {
    Serial.printf("H%u=%u CHANGE\n", (unsigned)addr, (unsigned)v);
  } else if (v != 0 || printZeros) {
    Serial.printf("H%u=%u\n", (unsigned)addr, (unsigned)v);
  }
  if (v != 0 && looksFaultCode(v)) {
    Serial.printf("ERR_ADDR=%u raw=%u\n", (unsigned)addr, (unsigned)v);
    Serial.printf(
        "ERROR_ADDR holding=%u raw=%u (holding error code, not Modbus EXC)\n",
        (unsigned)addr, (unsigned)v);
  } else if (v != 0) {
    // Nonzero mimo klasický fault pás — stále Hnnn= (výše); tag pro lov
    Serial.printf("ERR_ADDR=%u raw=%u\n", (unsigned)addr, (unsigned)v);
  }
}

// Quiet error map 500–520: print on first OK / CHANGE / nonzero (not every zero poll).
static void emitH500BlockWatch(const uint16_t *vals, bool force) {
  if (!vals) return;
  bool anyChange = !haveH500;
  bool anyNz = false;
  for (uint16_t i = 0; i < MB_H500_QTY; i++) {
    if (haveH500 && vals[i] != lastH500[i]) anyChange = true;
    if (vals[i] != 0) anyNz = true;
  }
  if (!force && !anyChange && !anyNz) {
    for (uint16_t i = 0; i < MB_H500_QTY; i++) lastH500[i] = vals[i];
    haveH500 = true;
    return;
  }

  for (uint16_t i = 0; i < MB_H500_QTY; i++) {
    uint16_t addr = (uint16_t)(MB_H500_ADDR + i);
    uint16_t v = vals[i];
    bool ch = haveH500 && v != lastH500[i];
    // Quiet: never print steady zeros (force = still only CHANGE/nonzero)
    if (ch || v != 0 || (force && v != 0)) {
      emitErrHoldingQuiet(addr, v, ch, false);
    }
    lastH500[i] = v;
  }
  haveH500 = true;
}

// FC03 500–520; quiet emit; keep last on busy/TO.
static void pollH500ErrorBlock(bool forcePrint) {
  uint16_t v500[MB_H500_QTY] = {0};
  uint8_t exc = 0;
  MbQuietStatus st =
      mbReadHoldingsQuietRetry(MB_H500_BLOCK, v500, MB_H500_QTY, &exc);
  if (st == MbQuietStatus::Ok) {
    emitH500BlockWatch(v500, forcePrint);
  }
}

// Read one ERR window; print only nonzero (or CHANGE vs lastH500 for 500–520).
// Returns nonzero count (0 on TO/fail).
static size_t dumpOneErrWindow(const ErrWin &w, bool annotateSparse) {
  if (w.qty == 0 || w.qty > MB_ERR_DUMP_MAX_QTY) return 0;
  MbReadReq req = {0x03, w.start, w.qty, w.label};
  uint16_t vals[MB_ERR_DUMP_MAX_QTY];
  memset(vals, 0, sizeof(vals));
  uint8_t exc = 0;
  MbQuietStatus st = mbReadHoldingsQuietRetry(req, vals, w.qty, &exc);
  if (st != MbQuietStatus::Ok) {
    if (mbVerbose) {
      Serial.printf("  ERR_WIN %s fail st=%u\n", w.label, (unsigned)st);
    }
    return 0;
  }

  size_t nz = 0;
  for (uint16_t i = 0; i < w.qty; i++) {
    uint16_t addr = (uint16_t)(w.start + i);
    uint16_t v = vals[i];
    if (v == 0) {
      // Keep 500–520 last cache in sync when this window overlaps
      if (addr >= MB_H500_ADDR && addr < (uint16_t)(MB_H500_ADDR + MB_H500_QTY)) {
        uint16_t idx = (uint16_t)(addr - MB_H500_ADDR);
        lastH500[idx] = 0;
        haveH500 = true;
      }
      continue;
    }
    nz++;
    bool ch = false;
    if (addr >= MB_H500_ADDR && addr < (uint16_t)(MB_H500_ADDR + MB_H500_QTY)) {
      uint16_t idx = (uint16_t)(addr - MB_H500_ADDR);
      ch = haveH500 && lastH500[idx] != v;
      lastH500[idx] = v;
      haveH500 = true;
    }
    emitErrHoldingQuiet(addr, v, ch, false);
    if (annotateSparse) {
      Serial.printf("ERR_SPARSE win=%s H%u=%u\n", w.label, (unsigned)addr,
                    (unsigned)v);
    }
  }
  return nz;
}

// One-shot candidate + sparse dump. Quiet: only nonzero / CHANGE / ERR_ADDR=.
// forceIgnoreCooldown: always after FC16 Start; false = H100 1→0 (cooldown).
static void dumpErrorCandidateWindows(const char *why, bool forceIgnoreCooldown) {
  if (mbErrDumpBusy) return;
  uint32_t now = millis();
  if (!forceIgnoreCooldown && mbLastErrDumpMs != 0 &&
      (now - mbLastErrDumpMs) < MB_ERR_DUMP_COOLDOWN_MS) {
    Serial.printf("ERR_DUMP skip (cooldown %lu ms) why=%s\n",
                  (unsigned long)MB_ERR_DUMP_COOLDOWN_MS,
                  why ? why : "?");
    return;
  }
  mbErrDumpBusy = true;
  mbLastErrDumpMs = now;
  bool prevBusy = mbScanBusy;
  mbScanBusy = true;

  Serial.println();
  Serial.printf("=== ERR_DUMP (%s) — quiet nonzero / ERR_ADDR= ===\n",
                why ? why : "?");
  Serial.println(
      F("wins: 0-31 100-120 200-220 400-420 500-520 1000-1020 2000-2020 + sparse"));

  size_t totalNz = 0;
  for (size_t i = 0; i < MB_ERR_WINS_N; i++) {
    Serial.printf("ERR_WIN %s…\n", MB_ERR_WINS[i].label);
    totalNz += dumpOneErrWindow(MB_ERR_WINS[i], false);
    delayPumpUi(MB_ERR_WIN_GAP_MS);
  }
  Serial.println(F("ERR_SPARSE…"));
  for (size_t i = 0; i < MB_ERR_SPARSE_N; i++) {
    totalNz += dumpOneErrWindow(MB_ERR_SPARSE[i], true);
    delayPumpUi(MB_ERR_WIN_GAP_MS);
  }

  if (totalNz == 0) {
    Serial.println(F("ERR_DUMP empty — no nonzero in candidate/sparse windows"));
  } else {
    Serial.printf("ERR_DUMP done nonzero_hits=%u\n", (unsigned)totalNz);
  }
  Serial.println(F("=== ERR_DUMP end ==="));

  mbScanBusy = prevBusy;
  mbErrDumpBusy = false;
  lastActivityMs = millis();
  silentAnnounced = false;
}

// FC03 → naplní vals[]; true při OK. Loguje TX/RX stejně jako poll.
static bool mbReadHoldingsCapture(const MbReadReq &r, uint16_t *vals, size_t valsCap,
                                  size_t *outN) {
  if (outN) *outN = 0;
  if (!vals || valsCap == 0) return mbOneRequest(r);

  uint8_t tx[8];
  size_t txn = mbBuildReadReq(tx, sizeof(tx), r);
  if (txn == 0) return false;

  flushRs485Rx();
  delay(2);
  flushRs485Rx();

  Serial.printf("[%lu ms] START_PROBE TX slave=%u %s | ",
                (unsigned long)millis(), (unsigned)MB_SLAVE, r.label);
  hexDump(tx, txn);
  Serial.println();

  size_t written = Serial1.write(tx, txn);
  Serial1.flush();
  if (written != txn) {
    Serial.printf("  START_PROBE TX_ERR wrote %u/%u\n", (unsigned)written,
                  (unsigned)txn);
  }
  delay(2);

  size_t expectMin = mbExpectedRespLen(r);
  if (expectMin < 5) expectMin = 5;
  uint8_t rx[MAX_FRAME];
  size_t rxn = mbReadResponse(rx, sizeof(rx), expectMin, MB_RESPONSE_TIMEOUT_MS);

  if (rxn == 0) {
    mbTimeoutCount++;
    Serial.println(F("  START_PROBE RX TIMEOUT"));
    return false;
  }

  Serial.print(F("  START_PROBE RX len="));
  Serial.print((unsigned)rxn);
  Serial.print(F(" | "));
  hexDump(rx, rxn);
  Serial.println();

  if (rxn < 5) {
    mbTimeoutCount++;
    Serial.println(F("  START_PROBE RX prilis kratka"));
    return false;
  }

  uint16_t got = (uint16_t)rx[rxn - 2] | ((uint16_t)rx[rxn - 1] << 8);
  uint16_t calc = modbusCrc16(rx, rxn - 2);
  if (got != calc) {
    Serial.printf("  START_PROBE CRC_ERR got=0x%04X calc=0x%04X\n", got, calc);
    return false;
  }

  if (rx[1] & 0x80) {
    Serial.printf("  START_PROBE EXC FC=0x%02X code=%u\n", (unsigned)rx[1],
                  (unsigned)rx[2]);
    mbOkCount++;
    return true;
  }

  if (rx[1] != 0x03) {
    Serial.printf("  START_PROBE FC_ERR odpoved FC=0x%02X\n", (unsigned)rx[1]);
    return false;
  }

  mbOkCount++;
  Serial.printf("  START_PROBE OK %s\n", r.label);
  mbLogHoldingData(r, rx, rxn, /*collect=*/false, /*dump=*/true);

  uint8_t bc = rx[2];
  size_t n = 0;
  for (uint8_t i = 0; i + 1 < bc && (3 + i + 1) < rxn - 2 && n < valsCap; i += 2) {
    vals[n++] = ((uint16_t)rx[3 + i] << 8) | rx[3 + i + 1];
  }
  if (outN) *outN = n;
  return true;
}

// Parse write response after TX; always logs RX hex. expectFc = 0x06 or 0x10.
static MbWriteResult mbParseWriteResponse(const uint8_t *rx, size_t rxn,
                                          uint8_t expectFc, const char *tag,
                                          uint16_t expectAddr, uint16_t expectVal,
                                          bool checkValueEcho) {
  if (!tag) tag = "WRITE";
  if (rxn == 0) {
    mbTimeoutCount++;
    Serial.printf("  %s RX TIMEOUT — silence (no ACK/EXC)\n", tag);
    return MbWriteResult::Timeout;
  }

  Serial.printf("  %s RX len=%u | ", tag, (unsigned)rxn);
  hexDump(rx, rxn);
  Serial.println();

  if (rxn < 5) {
    mbTimeoutCount++;
    Serial.printf("  %s RX too short\n", tag);
    return MbWriteResult::Timeout;
  }

  uint16_t got = (uint16_t)rx[rxn - 2] | ((uint16_t)rx[rxn - 1] << 8);
  uint16_t calc = modbusCrc16(rx, rxn - 2);
  if (got != calc) {
    Serial.printf("  %s CRC_ERR got=0x%04X calc=0x%04X\n", tag, got, calc);
    return MbWriteResult::CrcErr;
  }

  if (rx[0] != MB_SLAVE) {
    Serial.printf("  %s slave_id RX=%u expected=%u\n", tag, (unsigned)rx[0],
                  (unsigned)MB_SLAVE);
  }

  if (rx[1] & 0x80) {
    uint8_t code = rx[2];
    mbOkCount++;  // bus alive
    Serial.printf("  %s EXC FC=0x%02X code=%u (%s)\n", tag, (unsigned)rx[1],
                  (unsigned)code,
                  code == MB_EXC_SLAVE_BUSY ? "Slave Device Busy" : "exception");
    return (code == MB_EXC_SLAVE_BUSY) ? MbWriteResult::ExcBusy
                                       : MbWriteResult::ExcOther;
  }

  if (rx[1] != expectFc) {
    Serial.printf("  %s FC_ERR got=0x%02X expected=0x%02X\n", tag,
                  (unsigned)rx[1], (unsigned)expectFc);
    return MbWriteResult::FrameErr;
  }

  if (expectFc == 0x06) {
    if (rxn < 8) return MbWriteResult::FrameErr;
    uint16_t a = ((uint16_t)rx[2] << 8) | rx[3];
    uint16_t v = ((uint16_t)rx[4] << 8) | rx[5];
    mbOkCount++;
    if (a != expectAddr || (checkValueEcho && v != expectVal)) {
      Serial.printf("  %s ACK echo mismatch addr=%u val=%u (wanted %u/%u)\n",
                    tag, (unsigned)a, (unsigned)v, (unsigned)expectAddr,
                    (unsigned)expectVal);
      return MbWriteResult::FrameErr;
    }
    Serial.printf("  %s ACK FC06 holding[%u]=%u (echo CRC OK)\n", tag,
                  (unsigned)a, (unsigned)v);
    return MbWriteResult::Ack;
  }

  // FC16 response: slave FC addr(2) qty(2) CRC
  if (rxn < 8) return MbWriteResult::FrameErr;
  uint16_t a = ((uint16_t)rx[2] << 8) | rx[3];
  uint16_t q = ((uint16_t)rx[4] << 8) | rx[5];
  mbOkCount++;
  if (a != expectAddr) {
    Serial.printf("  %s ACK echo addr mismatch %u (wanted %u) qty=%u\n", tag,
                  (unsigned)a, (unsigned)expectAddr, (unsigned)q);
    return MbWriteResult::FrameErr;
  }
  Serial.printf("  %s ACK FC16 holding[%u] qty=%u (echo CRC OK)\n", tag,
                (unsigned)a, (unsigned)q);
  return MbWriteResult::Ack;
}

// FC06 Write Single — optional quiet-bus; always full TX/RX hex under tag
static MbWriteResult mbWriteSingleRegisterTagged(uint16_t addr, uint16_t value,
                                                 const char *tag,
                                                 bool doQuietBus) {
  uint8_t tx[8];
  size_t txn = mbBuildWriteSingle(tx, sizeof(tx), addr, value);
  if (txn == 0) return MbWriteResult::TxErr;
  if (!tag) tag = "FC06";

  if (doQuietBus) {
    Serial.printf("  %s quiet-bus ≥%lu ms (poll paused)…\n", tag,
                  (unsigned long)MB_QUIET_BUS_MS);
    waitBusQuiet(MB_QUIET_BUS_MS, MB_QUIET_BUS_MAX_MS);
  } else {
    flushRs485Rx();
    delay(2);
    flushRs485Rx();
  }

  Serial.printf("[%lu ms] %s TX slave=%u FC06 holding[%u]=%u | ",
                (unsigned long)millis(), tag, (unsigned)MB_SLAVE, (unsigned)addr,
                (unsigned)value);
  hexDump(tx, txn);
  Serial.println();

  size_t written = Serial1.write(tx, txn);
  Serial1.flush();
  if (written != txn) {
    Serial.printf("  %s TX_ERR wrote %u/%u\n", tag, (unsigned)written,
                  (unsigned)txn);
    return MbWriteResult::TxErr;
  }
  delay(2);

  uint8_t rx[MAX_FRAME];
  size_t rxn = mbReadResponse(rx, sizeof(rx), 5, MB_RESPONSE_TIMEOUT_MS);
  return mbParseWriteResponse(rx, rxn, 0x06, tag, addr, value, true);
}

static bool mbWriteSingleRegister(uint16_t addr, uint16_t value) {
  return mbWriteSingleRegisterTagged(addr, value, "START_PROBE", false) ==
         MbWriteResult::Ack;
}

// FC16 write qty registers starting at addr
static MbWriteResult mbWriteMultipleRegisterTagged(uint16_t addr, uint16_t qty,
                                                   const uint16_t *vals,
                                                   const char *tag,
                                                   bool doQuietBus) {
  uint8_t tx[32];
  size_t txn = mbBuildWriteMultiple(tx, sizeof(tx), addr, qty, vals);
  if (txn == 0) return MbWriteResult::TxErr;
  if (!tag) tag = "FC16";

  if (doQuietBus) {
    Serial.printf("  %s quiet-bus ≥%lu ms (poll paused)…\n", tag,
                  (unsigned long)MB_QUIET_BUS_MS);
    waitBusQuiet(MB_QUIET_BUS_MS, MB_QUIET_BUS_MAX_MS);
  } else {
    flushRs485Rx();
    delay(2);
    flushRs485Rx();
  }

  Serial.printf("[%lu ms] %s TX slave=%u FC16 holding[%u] qty=%u val0=%u | ",
                (unsigned long)millis(), tag, (unsigned)MB_SLAVE, (unsigned)addr,
                (unsigned)qty, (unsigned)(vals ? vals[0] : 0));
  hexDump(tx, txn);
  Serial.println();

  size_t written = Serial1.write(tx, txn);
  Serial1.flush();
  if (written != txn) {
    Serial.printf("  %s TX_ERR wrote %u/%u\n", tag, (unsigned)written,
                  (unsigned)txn);
    return MbWriteResult::TxErr;
  }
  delay(2);

  uint8_t rx[MAX_FRAME];
  size_t rxn = mbReadResponse(rx, sizeof(rx), 5, MB_RESPONSE_TIMEOUT_MS);
  return mbParseWriteResponse(rx, rxn, 0x10, tag, addr, vals ? vals[0] : 0,
                              false);
}

// Toggle target: panel Start shows 1→0 — write 0 when H100==1, else write 1.
static uint16_t h100ToggleValue() {
  if (haveH100 && lastH100 == MB_H100_START_VALUE) return 0;
  return MB_H100_START_VALUE;
}

static void h100WriteBeginWindow(const char *why) {
  mbScanBusy = true;  // pause fast FC03 for whole write window
  // Soft defer only — EndWindow always clears busy + resumes (never forever).
  mbNextFastMs = millis() + MB_H100_CONFIRM_EARLY_MS + MB_H100_CONFIRM_LATE_MS +
                 MB_H100_POLL_RESUME_MS;
  Serial.printf("  poll PAUSED (%s)\n", why ? why : "H100 write");
}

// One confirm sample: FC03 holding 100–101 + H1; always print; update UI.
// expectWrite: value we just FC16'd — print "(confirm)" even when unchanged.
static void h100ConfirmSample(uint16_t expectWrite, const char *phase) {
  Serial.printf("H100 confirm %s (expect=%u)…\n", phase ? phase : "?",
                (unsigned)expectWrite);

  uint16_t v100[2] = {0, 0};
  uint8_t exc = 0;
  MbQuietStatus st100 =
      mbReadHoldingsQuietRetry(MB_H100_BLOCK, v100, 2, &exc);
  if (st100 == MbQuietStatus::Ok) {
    emitHoldingWatchForce("H100", v100[0], &lastH100, &haveH100,
                          v100[0] == expectWrite);
    emitHoldingWatch("H101", v100[1], &lastH101, &haveH101);
  } else {
    // Always visible after write — bypass rate-limit once for confirm path.
    if (st100 == MbQuietStatus::ExcBusy) {
      Serial.println(F("H100 confirm busy/ex6"));
    } else if (st100 == MbQuietStatus::Timeout) {
      Serial.println(F("H100 confirm busy/TO"));
    } else {
      logH100BusyOnce(st100, exc);
    }
  }

  delayPumpUi(MB_FAST_INTER_MS);

  uint16_t v1 = 0;
  uint8_t exc1 = 0;
  MbQuietStatus st1 = mbReadHoldingsQuietRetry(MB_H1_REQ, &v1, 1, &exc1);
  if (st1 == MbQuietStatus::Ok) {
    emitHoldingWatch("H1", v1, &lastH1, &haveH1);
  }

  uiTestUpdate(lastH1, haveH1, lastH100, haveH100, lastH101, haveH101, nullptr);
  lastActivityMs = millis();
  silentAnnounced = false;
}

// After FC16/FC06: dual FC03 confirm (catch write + auto-clear 1→0), resume poll.
static void h100WriteEndWindow(uint16_t expectWrite) {
  // Sample 1 early — catch holding=1 before pump auto-clears.
  delayPumpUi(MB_H100_CONFIRM_EARLY_MS);
  h100ConfirmSample(expectWrite, "1/2");

  // Sample 2 — if pump cleared 1→0, print CHANGE (both values visible).
  delayPumpUi(MB_H100_CONFIRM_LATE_MS);
  h100ConfirmSample(expectWrite, "2/2");

  // Fault latch often clears with H100 1→0 — dump candidate windows once now.
  dumpErrorCandidateWindows("post-FC16 Start", true);

  mbScanBusy = false;
  mbNextFastMs = millis() + MB_H100_POLL_RESUME_MS;
  Serial.println(F("  poll RESUMED (post-write confirm + ERR_DUMP)"));
  lastActivityMs = millis();
  silentAnnounced = false;
}

// UI / Serial h100 / fc16: quiet-bus + FC16 qty1 toggle (primary). Full TX/RX.
// Milan: FC06 na této pumpě nic nedělá — Start path = FC16 only.
static void runH100StartWrite(bool fromUi) {
  uint16_t value = h100ToggleValue();

  Serial.println();
  Serial.printf("=== H100_START (%s) ===\n", fromUi ? "UI" : "Serial");
  if (haveH100) {
    Serial.printf(
        "plan: quiet-bus + FC16 qty1 slave=%u holding[%u]=%u (toggle from H100=%u)\n",
        (unsigned)MB_SLAVE, (unsigned)MB_H100_START_ADDR, (unsigned)value,
        (unsigned)lastH100);
  } else {
    Serial.printf(
        "plan: quiet-bus + FC16 qty1 slave=%u holding[%u]=%u (H100 unknown → write 1)\n",
        (unsigned)MB_SLAVE, (unsigned)MB_H100_START_ADDR, (unsigned)value);
  }
  Serial.println(F("Milan: panel Start → H100 CHANGE 1→0; expect ACK or EXC/TIMEOUT below."));
  Serial.println(F("Start = FC16 only (FC06 na teto pumpe nic nedela)."));
  Serial.println(F("Neni prepínac — napis: fc16   nebo  h100alt"));

  char st[96];
  snprintf(st, sizeof(st), "quiet+FC16 H100=%u …", (unsigned)value);
  uiTestSetStatus(st);

  h100WriteBeginWindow("H100_START");
  uint16_t v = value;
  MbWriteResult wr =
      mbWriteMultipleRegisterTagged(MB_H100_START_ADDR, 1, &v, "H100_START", true);
  Serial.printf("H100_START result=%s (FC16 qty1 val=%u)\n", mbWriteResultStr(wr),
                (unsigned)value);

  // Observe only: dual confirm prints H100= even if == write; auto-clear 1→0 = both.
  h100WriteEndWindow(value);

  if (wr == MbWriteResult::Ack) {
    snprintf(st, sizeof(st), "H100 FC16=%u ACK H100=%u", (unsigned)value,
             (unsigned)(haveH100 ? lastH100 : 0));
  } else {
    snprintf(st, sizeof(st), "H100 FC16=%u %s — see Serial TX/RX",
             (unsigned)value, mbWriteResultStr(wr));
  }
  uiTestUpdate(lastH1, haveH1, lastH100, haveH100, lastH101, haveH101, st);
}

// Serial fc16 / h100fc16 [0|1]: same FC16 path; no arg = toggle (alias of h100).
static void runH100Fc16Oneshoot(bool haveExplicit, uint16_t value) {
  if (!haveExplicit) {
    runH100StartWrite(false);
    return;
  }

  Serial.println();
  Serial.println(F("=== H100_FC16 (one-shot qty1) ==="));
  Serial.println(F("Start = FC16 only (FC06 na teto pumpe nic nedela)."));
  Serial.println(F("Neni prepínac — napis: fc16   nebo  h100alt"));
  Serial.printf(
      "plan: quiet-bus + FC16 qty1 slave=%u holding[%u]=%u (explicit)\n",
      (unsigned)MB_SLAVE, (unsigned)MB_H100_START_ADDR, (unsigned)value);

  char st[96];
  snprintf(st, sizeof(st), "quiet+FC16 H100=%u …", (unsigned)value);
  uiTestSetStatus(st);

  h100WriteBeginWindow("H100_FC16");
  uint16_t v = value;
  MbWriteResult wr =
      mbWriteMultipleRegisterTagged(MB_H100_START_ADDR, 1, &v, "H100_FC16", true);
  Serial.printf("H100_FC16 result=%s (FC16 qty1 val=%u)\n", mbWriteResultStr(wr),
                (unsigned)value);
  h100WriteEndWindow(value);

  if (wr == MbWriteResult::Ack) {
    snprintf(st, sizeof(st), "H100 FC16=%u ACK H100=%u", (unsigned)value,
             (unsigned)(haveH100 ? lastH100 : 0));
  } else {
    snprintf(st, sizeof(st), "H100 FC16=%u %s — see Serial", (unsigned)value,
             mbWriteResultStr(wr));
  }
  uiTestUpdate(lastH1, haveH1, lastH100, haveH100, lastH101, haveH101, st);
}

// Serial fc06 [0|1]: experimentální FC06 (na této pumpě typicky nic).
static void runH100Fc06Oneshoot(bool haveExplicit, uint16_t value) {
  if (!haveExplicit) value = h100ToggleValue();

  Serial.println();
  Serial.println(F("=== H100_FC06 (experiment — FC06 usually does nothing) ==="));
  Serial.println(F("Start primary = FC16. Neni prepínac — napis: fc16   nebo  h100alt"));
  Serial.printf(
      "plan: quiet-bus + FC06 slave=%u holding[%u]=%u%s\n",
      (unsigned)MB_SLAVE, (unsigned)MB_H100_START_ADDR, (unsigned)value,
      haveExplicit ? " (explicit)" : " (toggle)");

  char st[96];
  snprintf(st, sizeof(st), "quiet+FC06 H100=%u …", (unsigned)value);
  uiTestSetStatus(st);

  h100WriteBeginWindow("H100_FC06");
  MbWriteResult wr =
      mbWriteSingleRegisterTagged(MB_H100_START_ADDR, value, "H100_FC06", true);
  Serial.printf("H100_FC06 result=%s (FC06 val=%u) — primary Start is FC16\n",
                mbWriteResultStr(wr), (unsigned)value);
  h100WriteEndWindow(value);

  snprintf(st, sizeof(st), "FC06=%u %s (use fc16)", (unsigned)value,
           mbWriteResultStr(wr));
  uiTestUpdate(lastH1, haveH1, lastH100, haveH100, lastH101, haveH101, st);
}

// Serial h100alt: experimentální sekvence — FC16 first (primary), FC06 jen srovnání
static void runH100AltMethods() {
  Serial.println();
  Serial.println(F("=== H100_ALT (FC16 primary; FC06 experiment) ==="));
  Serial.println(F("Each step: pause poll → quiet bus → write → wait echo → settle."));
  Serial.println(F("Start = FC16 only (FC06 na teto pumpe nic nedela)."));
  if (haveH100) {
    Serial.printf("slave=%u addr=%u (0-based holding 100) H100_now=%u\n",
                  (unsigned)MB_SLAVE, (unsigned)MB_H100_START_ADDR,
                  (unsigned)lastH100);
  } else {
    Serial.printf("slave=%u addr=%u (0-based holding 100) H100_now=?\n",
                  (unsigned)MB_SLAVE, (unsigned)MB_H100_START_ADDR);
  }

  uiTestSetStatus("H100_ALT methods…");
  h100WriteBeginWindow("H100_ALT");

  struct {
    const char *name;
    uint8_t fc;  // 6 or 16
    uint16_t value;
  } steps[] = {
      {"FC16 qty1=1", 16, 1},
      {"FC16 qty1=0", 16, 0},
      {"FC06=1 (exp)", 6, 1},
      {"FC06=0 (exp)", 6, 0},
  };
  const size_t nSteps = sizeof(steps) / sizeof(steps[0]);

  for (size_t i = 0; i < nSteps; i++) {
    Serial.println();
    Serial.printf("--- H100_ALT [%u/%u] %s ---\n", (unsigned)(i + 1),
                  (unsigned)nSteps, steps[i].name);
    MbWriteResult wr;
    if (steps[i].fc == 16) {
      uint16_t v = steps[i].value;
      wr = mbWriteMultipleRegisterTagged(MB_H100_START_ADDR, 1, &v, "H100_ALT",
                                         true);
    } else {
      wr = mbWriteSingleRegisterTagged(MB_H100_START_ADDR, steps[i].value,
                                       "H100_ALT", true);
    }
    Serial.printf("H100_ALT [%s] → %s\n", steps[i].name, mbWriteResultStr(wr));
    delayPumpUi(MB_H100_WRITE_SETTLE_MS);
  }

  Serial.println();
  Serial.println(F("=== H100_ALT done — compare ACK / EXC6 / TIMEOUT lines above ==="));
  h100WriteEndWindow(steps[nSteps - 1].value);
  uiTestUpdate(lastH1, haveH1, lastH100, haveH100, lastH101, haveH101,
               "H100_ALT done — see Serial");
}

// START_PROBE: nejdřív jen READ 0–31; FC06 jen na kandidáty s raw==0 (ne slepě [0]).
static void runStartProbe() {
  flushRs485Rx();

  uint16_t holds[MB_HOLD_QTY];
  size_t holdN = 0;
  memset(holds, 0, sizeof(holds));

  Serial.println();
  Serial.println(F("=== START_PROBE (EXPERIMENTALNI) ==="));
  Serial.println(F("POZOR: FC06 muze zmenit rezim TČ. Power Spawer = ? (FC01 coil TO)."));
  Serial.println(F("Krok1: FC03 holding 0-31 (nic nezapisujeme)."));
  Serial.println(F("Krok2: FC06=1 jen na kandidaty s aktualni hodnotou 0."));
  Serial.println(F("holding[0] live casto ~200 — spis REZIM/setpoint, NE power on/off."));
  Serial.println(F("Chyba na panelu = jednotka uz bezi; start pres Modbus nemusi nic delat."));
  Serial.println(F("FC01 coil timeout => power muze byt PANEL-ONLY."));

  Serial.println(F("--- START_PROBE pred (holding 0-31) ---"));
  bool got = mbReadHoldingsCapture(MB_START_BEFORE, holds, MB_HOLD_QTY, &holdN);
  delay(MB_INTER_REQ_MS);

  if (!got || holdN == 0) {
    Serial.println(F("START_PROBE: pred-read selhal — FC06 SKIP."));
    Serial.println(F("=== START_PROBE hotovo ==="));
    lastActivityMs = millis();
    silentAnnounced = false;
    mbNextFastMs = millis() + MB_FAST_POLL_MS;
    mbNextPollMs = millis() + MB_POLL_INTERVAL_MS;
    return;
  }

  Serial.print(F("START_PROBE summary holding 0-15:"));
  for (size_t i = 0; i < 16 && i < holdN; i++) {
    Serial.printf(" [%u]=%u", (unsigned)i, (unsigned)holds[i]);
  }
  Serial.println();
  if (holdN > 16) {
    Serial.print(F("START_PROBE summary holding 16-31:"));
    for (size_t i = 16; i < holdN && i < MB_HOLD_QTY; i++) {
      Serial.printf(" [%u]=%u", (unsigned)i, (unsigned)holds[i]);
    }
    Serial.println();
  }

  uint16_t h0 = holds[0];
  if (h0 > 1) {
    Serial.printf(
        "START_PROBE: holding[0]=%u > 1 — NEZAPISUJI na [0] (neni 0/1 power flag).\n",
        (unsigned)h0);
  } else if (h0 == 1) {
    Serial.println(F("START_PROBE: holding[0]=1 uz — mozna uz 'on'/rezim; FC06 [0] SKIP."));
  } else {
    Serial.println(F("START_PROBE: holding[0]=0 — zkusim FC06 [0]=1 jako kandidata."));
  }

  Serial.println(F("--- START_PROBE FC06 kandidati (jen raw==0) ---"));
  size_t tried = 0;
  size_t okWrites = 0;

  // Volitelně holding[0] jen když je opravdu 0/1-style
  if (h0 == 0) {
    Serial.println(F("START_PROBE try holding[0]=1"));
    if (mbWriteSingleRegister(0, MB_START_WRITE_VALUE)) okWrites++;
    tried++;
    delay(MB_START_SETTLE_MS);
  }

  for (size_t i = 0; i < MB_START_CANDIDATES_N; i++) {
    uint16_t addr = MB_START_CANDIDATES[i];
    if (addr >= holdN) {
      Serial.printf("START_PROBE skip holding[%u] — mimo nacteny rozsah\n",
                    (unsigned)addr);
      continue;
    }
    uint16_t cur = holds[addr];
    if (cur != 0) {
      Serial.printf("START_PROBE skip holding[%u]=%u (neni 0 — nenicim)\n",
                    (unsigned)addr, (unsigned)cur);
      continue;
    }
    Serial.printf("START_PROBE try holding[%u]=%u (bylo 0)\n", (unsigned)addr,
                  (unsigned)MB_START_WRITE_VALUE);
    if (mbWriteSingleRegister(addr, MB_START_WRITE_VALUE)) okWrites++;
    tried++;
    delay(MB_START_SETTLE_MS);
  }

  if (tried == 0) {
    Serial.println(F("START_PROBE: zadny FC06 — zadny kandidat s raw==0 / [0] skip."));
    Serial.println(F("  => power Spawer muze byt PANEL-ONLY (nebo jina mapa mimo 0-31)."));
  } else {
    Serial.printf("START_PROBE: FC06 pokusu=%u OK_echo=%u\n", (unsigned)tried,
                  (unsigned)okWrites);
  }

  delay(MB_INTER_REQ_MS);
  Serial.println(F("--- START_PROBE po (holding 0-31) ---"));
  mbReadHoldingsCapture(MB_START_AFTER, holds, MB_HOLD_QTY, &holdN);

  Serial.println(F("=== START_PROBE hotovo — porovnej pred/po + panel Spaweru ==="));
  lastActivityMs = millis();
  silentAnnounced = false;
  mbNextFastMs = millis() + MB_FAST_POLL_MS;
  mbNextPollMs = millis() + MB_POLL_INTERVAL_MS;
}

static void runModbusPollCycle() {
  mbScanBusy = true;
  flushRs485Rx();
  scanCandReset();

  mbCycleCount++;
  if (mbVerbose) {
    Serial.println();
    Serial.printf(
        "=== FC03 scan cyklus %lu (lehky; slave %u @ %lu 8N1; 10-31 + 0/500/2000) ===\n",
        (unsigned long)mbCycleCount, (unsigned)MB_SLAVE, (unsigned long)busBaud);
    Serial.printf(
        "  raw %u..%u → TEMP_ADDR | hold %u..%u raw=%u → ERROR_ADDR | mezireq %lu ms\n",
        (unsigned)CANDIDATE_TEMP_MIN, (unsigned)CANDIDATE_TEMP_MAX,
        (unsigned)ERROR_HOLD_MIN, (unsigned)ERROR_HOLD_MAX,
        (unsigned)ERROR_RAW_MATCH, (unsigned long)MB_INTER_REQ_MS);
  }

  size_t okBlocks = 0;
  size_t failBlocks = 0;

  for (size_t i = 0; i < MB_REQS_N; i++) {
    if (mbReqSkip[i]) continue;

    bool ok = mbOneRequest(MB_REQS[i]);
    if (ok) {
      okBlocks++;
    } else {
      failBlocks++;
      // FC03 fail na vysokých adresách = možná mimo mapu — držíme se scanu
      if (MB_REQS[i].fc != 0x03) {
        mbReqSkip[i] = true;
        if (mbVerbose) {
          Serial.printf("  SKIP dalsi cykly: %s (timeout/fail — spam OFF)\n",
                        MB_REQS[i].label);
        }
      }
    }
    if (i + 1 < MB_REQS_N) delay(MB_INTER_REQ_MS);
  }

  printTempAddrSummary();
  printErrorAddrSummary();
  if (mbVerbose) {
    Serial.printf(
        "  souhrn bloky OK=%u FAIL=%u | celkem OK=%lu TO=%lu (pasivni sniff)\n",
        (unsigned)okBlocks, (unsigned)failBlocks, (unsigned long)mbOkCount,
        (unsigned long)mbTimeoutCount);
  }

  lastActivityMs = millis();
  silentAnnounced = false;
  mbNextPollMs = millis() + MB_POLL_INTERVAL_MS;
  mbNextFastMs = millis() + MB_FAST_POLL_MS;
  mbScanBusy = false;
}

// Fast ~1 s: H100-101 FIRST (priorita), settle, pak H1.
// EXC 0x06 / TO: keep last good; retry+backoff; quiet log once (ne „fail“ flood).
static void pollFastHoldings() {
  if (!mbPollEnabled || mbScanBusy) return;
  if ((int32_t)(millis() - mbNextFastMs) < 0) return;
  mbNextFastMs = millis() + MB_FAST_POLL_MS;

  // Priorita: blok 100–101 dřív než H1 (Milan H100 reliability)
  uint16_t v100[2] = {0, 0};
  uint8_t exc100 = 0;
  bool h100Cleared10 = false;
  MbQuietStatus st100 =
      mbReadHoldingsQuietRetry(MB_H100_BLOCK, v100, 2, &exc100);
  if (st100 == MbQuietStatus::Ok) {
    // Panel Start fault: H100 1→0 — catch error latch with one ERR_DUMP
    if (haveH100 && lastH100 == MB_H100_START_VALUE && v100[0] == 0) {
      h100Cleared10 = true;
    }
    emitHoldingWatch("H100", v100[0], &lastH100, &haveH100);
    emitHoldingWatch("H101", v100[1], &lastH101, &haveH101);
  } else {
    // NEVER clear haveH100/haveH101 — UI drží last good; log quietly.
    logH100BusyOnce(st100, exc100);
  }

  // Stagger: H1 ne hned po H100 (hlavně po busy) — bus settle.
  uint32_t settle = MB_FAST_INTER_MS;
  if (st100 == MbQuietStatus::ExcBusy || st100 == MbQuietStatus::Timeout) {
    settle = MB_FAST_INTER_MS + MB_BUSY_BACKOFF_MS;
  }
  delayPumpUi(settle);

  uint16_t v1 = 0;
  uint8_t exc1 = 0;
  MbQuietStatus st1 = mbReadHoldingsQuietRetry(MB_H1_REQ, &v1, 1, &exc1);
  if (st1 == MbQuietStatus::Ok) {
    emitHoldingWatch("H1", v1, &lastH1, &haveH1);
  }
  // H1 busy/TO: keep haveH1; no flood (H100 log covers cycle when both busy)

  if (MB_H0_POLL_ENABLED) {
    delayPumpUi(MB_FAST_INTER_MS);
    uint16_t v0 = 0;
    if (mbReadHoldingsQuiet(MB_H0_REQ, &v0, 1)) {
      emitHoldingWatch("H0", v0, &lastH0, &haveH0);
    }
  }

  // Light continuous: FC03 holding 500–520 (quiet CHANGE/nonzero)
  delayPumpUi(MB_FAST_INTER_MS);
  pollH500ErrorBlock(false);

  // Panel Start → H100 1→0: dump candidate error windows once (cooldown)
  if (h100Cleared10) {
    dumpErrorCandidateWindows("H100 1->0", false);
  }

  // UI: labels only redraw when values change (no per-poll flicker)
  // have* flags stay true after first OK — busy nevrací „—“ / unknown.
  uiTestUpdate(lastH1, haveH1, lastH100, haveH100, lastH101, haveH101, nullptr);
  uiTestTick();

  lastActivityMs = millis();
  silentAnnounced = false;
}

static void pollModbusMaster() {
  if (!mbPollEnabled) return;
  pollFastHoldings();
  if (!MB_LIGHT_SCAN_ENABLED) return;
  if (mbScanBusy) return;
  if ((int32_t)(millis() - mbNextPollMs) < 0) return;
  runModbusPollCycle();
}

static void pollAutoStartProbe() {
  if (!AUTO_START_PROBE_ENABLED) return;
  if (autoStartProbeDone) return;
  if ((int32_t)(millis() - autoStartProbeAtMs) < 0) return;
  autoStartProbeDone = true;
  Serial.println();
  Serial.println(F("=== AUTO START_PROBE (5s po bootu; BOOT GPIO0 na 7B = LCD/download) ==="));
  runStartProbe();
}

static void printLoopHelp() {
  Serial.println(F("--- loop / external RS485 check ---"));
  Serial.println(F("WRONG: short A to B (differential pair — not UART TX-RX loopback)."));
  Serial.println(F("OK: Waveshare A <-> USB-RS485 A,  B <-> USB-RS485 B (+GND)."));
  Serial.println(F("Spawer: A/B paralelne, terminace Waveshare OFF. Sleduj MODBUS_OK vs TIMEOUT."));
  Serial.println(F("Pri TIMEOUT: prohod A<->B. Echo = prikaz echo. START = auto 5s / start."));
}

static void printListenReminder() {
  Serial.println(F("Mode quiet: H100 + H1 + H101 + H500-520 ~1s (temp auto scan OFF)."));
  Serial.println(F("H100=/H1=/H101= + CHANGE (Milan: panel Start → H100 1→0). H0 OFF."));
  Serial.println(F("ERR_DUMP: po FC16 / H100 1→0 / errdump — Hnnn= + ERR_ADDR= (jen nonzero)."));
  Serial.println(F("Wins: 0-31 100-120 200-220 400-420 500-520 1000-1020 2000-2020 + sparse."));
  Serial.println(F("EXC6 Slave Busy → H100 busy/ex6 (keep last); ≠ ERR_ADDR raw=6."));
  Serial.println(F("LCD: H1×0.1°C + H100/H101 + Start → quiet-bus FC16 qty1 H100 (toggle)."));
  Serial.println(F("Start = FC16 only (FC06 na teto pumpe nic nedela)."));
  Serial.println(F("Neni prepínac — napis: fc16   nebo  h100alt"));
  Serial.println(F("verbose = dump + TEMP_ADDR/ERROR_ADDR. poll = jednorazovy scan."));
  Serial.println(F("Spawer A/B: H100 busy/TO? = polarita A<->B / baud / napajeni."));
  Serial.println(F("H100 write: UI Start / h100 / fc16 (=FC16); h100alt=seq. start=PROBE. BOOT=flash."));
}

static void printHelp() {
  Serial.println(F("Prikazy (USB 115200) — pokud PuTTY vstup funguje:"));
  Serial.println(F("  help | listen | b | baud | 4800 | 9600 | 19200 | 38400 | 57600 | 115200"));
  Serial.println(F("  poll | p        — jeden FC03 temp/error scan ted (verbose doporuceno)"));
  Serial.println(F("  errdump | err   — jednou ERR_DUMP kandidatnich oken (Hnnn=/ERR_ADDR=)"));
  Serial.println(F("  h100 | start100 | fc16 | h100fc16 — quiet-bus FC16 qty1 H100 toggle"));
  Serial.println(F("  fc16 0 | fc16 1 — same FC16 path, explicit value"));
  Serial.println(F("  h100alt         — FC16 first, then FC06 experiment (ACK vs EXC/TO)"));
  Serial.println(F("  fc06 [0|1]      — experiment FC06 (na teto pumpe typicky nic)"));
  Serial.println(F("  Start = FC16 only (FC06 na teto pumpe nic nedela)."));
  Serial.println(F("  Neni prepínac — napis: fc16   nebo  h100alt"));
  Serial.println(F("  start | s       — START_PROBE FC06 kandidati 0-31 (auto OFF)"));
  Serial.println(F("  nopol           — vypni periodicky poll (fast)"));
  Serial.println(F("  pollon          — zapni fast H100/H1/H101/H500-520 ~1s"));
  Serial.println(F("  quiet           — H1/H100/H101 + H500-520 + ERR_DUMP triggers (default)"));
  Serial.println(F("  verbose | v     — dump hex + TEMP_ADDR / ERROR_ADDR + pasivni frames"));
  Serial.println(F("  echo | e | ping — TX test (NE na zivem Spawer, pokud nemusis)"));
  Serial.println(F("  echoon/echooff  — auto echo 3x po startu (default OFF ve Spawer)"));
  Serial.println(F("  tx | loop"));
  Serial.println(F("Fast ~1s: FC03 100-101, H1, 500-520 → H100=/H1=/H5xx=/ERR_ADDR=."));
  Serial.println(F("ERR_DUMP auto: po FC16 Start + pri H100 1→0 (cooldown 5s)."));
  Serial.println(F("LCD Start = H100_START tap + quiet-bus FC16 qty1 + TX/RX + confirm."));
  Serial.println(F("Auto TEMP scan OFF (nehladovi H100). Ručne: poll / verbose / errdump."));
  Serial.println(F("AUTO START_PROBE OFF. BOOT 7B = download. GPIO0 = LCD G3."));
  Serial.println(F("Pri SILENT/TIMEOUT: prohod A<->B, zkus jiny baud."));
}

static void applyBaud(uint32_t baud) {
  busBaud = baud;
  buflen = 0;
  Serial1.end();
  delay(20);
  Serial1.begin(busBaud, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  lastActivityMs = millis();
  lastByteUs = micros();
  silentAnnounced = false;
  Serial.printf("Bus baud -> %lu 8N1 (frame gap ~%lu us)\n",
                (unsigned long)busBaud, (unsigned long)frameGapUs());
}

static bool parseBaudToken(const char *tok, uint32_t *out) {
  if (!tok || !*tok) return false;
  char *end = nullptr;
  unsigned long v = strtoul(tok, &end, 10);
  if (end == tok || (end && *end != '\0')) return false;
  for (size_t i = 0; i < BAUD_TABLE_N; i++) {
    if (BAUD_TABLE[i] == v) {
      *out = (uint32_t)v;
      return true;
    }
  }
  return false;
}

static void handleCommand(char *line) {
  while (*line == ' ' || *line == '\t') line++;
  size_t n = strlen(line);
  while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
                   line[n - 1] == '\r' || line[n - 1] == '\n')) {
    line[--n] = '\0';
  }
  if (n == 0) return;

  for (size_t i = 0; line[i]; i++) {
    if (line[i] >= 'A' && line[i] <= 'Z') line[i] = (char)(line[i] - 'A' + 'a');
  }

  if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0 || strcmp(line, "h") == 0) {
    printHelp();
    return;
  }
  if (strcmp(line, "listen") == 0 || strcmp(line, "l") == 0) {
    printListenReminder();
    return;
  }
  if (strcmp(line, "b") == 0 || strcmp(line, "baud") == 0) {
    Serial.printf("Current bus baud: %lu 8N1\n", (unsigned long)busBaud);
    return;
  }
  if (strcmp(line, "poll") == 0 || strcmp(line, "p") == 0) {
    runModbusPollCycle();
    return;
  }
  if (strcmp(line, "errdump") == 0 || strcmp(line, "err") == 0 ||
      strcmp(line, "errors") == 0) {
    dumpErrorCandidateWindows("Serial errdump", true);
    return;
  }
  if (strcmp(line, "h100") == 0 || strcmp(line, "start100") == 0) {
    runH100StartWrite(false);
    return;
  }
  if (strcmp(line, "h100alt") == 0 || strcmp(line, "alt100") == 0) {
    runH100AltMethods();
    return;
  }
  // fc16 / h100fc16 [0|1] — same as h100 (FC16); no arg = toggle
  if (strncmp(line, "fc16", 4) == 0 || strncmp(line, "h100fc16", 8) == 0) {
    const char *rest = nullptr;
    if (strncmp(line, "h100fc16", 8) == 0) {
      rest = line + 8;
    } else {
      rest = line + 4;
    }
    while (*rest == ' ' || *rest == '\t') rest++;
    if (*rest == '\0') {
      runH100Fc16Oneshoot(false, 0);
      return;
    }
    if ((*rest == '0' || *rest == '1') && rest[1] == '\0') {
      runH100Fc16Oneshoot(true, (uint16_t)(*rest - '0'));
      return;
    }
    Serial.printf("Unknown: '%s' — type help (fc16 / fc16 0 / fc16 1)\n", line);
    return;
  }
  // fc06 [0|1] — experiment only (FC06 usually does nothing on this pump)
  if (strncmp(line, "fc06", 4) == 0) {
    const char *rest = line + 4;
    while (*rest == ' ' || *rest == '\t') rest++;
    if (*rest == '\0') {
      runH100Fc06Oneshoot(false, 0);
      return;
    }
    if ((*rest == '0' || *rest == '1') && rest[1] == '\0') {
      runH100Fc06Oneshoot(true, (uint16_t)(*rest - '0'));
      return;
    }
    Serial.printf("Unknown: '%s' — type help (fc06 / fc06 0 / fc06 1)\n", line);
    return;
  }
  if (strcmp(line, "start") == 0 || strcmp(line, "s") == 0) {
    runStartProbe();
    return;
  }
  if (strcmp(line, "nopol") == 0 || strcmp(line, "polloff") == 0) {
    mbPollEnabled = false;
    Serial.println(F("Modbus poll OFF — jen pasivni sniff."));
    return;
  }
  if (strcmp(line, "pollon") == 0) {
    mbPollEnabled = true;
    mbNextFastMs = millis();  // soon
    mbNextPollMs = millis() + MB_POLL_INTERVAL_MS;
    Serial.println(F("Modbus poll ON — H100/H1/H101 ~1s (auto scan OFF)."));
    return;
  }
  if (strcmp(line, "quiet") == 0 || strcmp(line, "q") == 0) {
    mbVerbose = false;
    Serial.println(F("Quiet ON — jen H1=/H100=/H101= [/CHANGE] + busy/ex6."));
    return;
  }
  if (strcmp(line, "verbose") == 0 || strcmp(line, "v") == 0) {
    mbVerbose = true;
    Serial.println(F("Verbose ON — dump + TEMP/ERROR + pasivni frames."));
    return;
  }
  if (strcmp(line, "echoon") == 0) {
    autoEchoEnabled = true;
    autoEchoStartMs = millis();
    autoEchoDone = 0;
    Serial.println(F("Auto echo ON — 3x (0/10/20 s). Na Spawer bus spis OFF."));
    return;
  }
  if (strcmp(line, "echooff") == 0) {
    autoEchoEnabled = false;
    autoEchoDone = AUTO_ECHO_COUNT;
    Serial.println(F("Auto echo OFF (default Spawer). START_PROBE = jen prikaz start."));
    return;
  }
  if (strcmp(line, "echo") == 0 || strcmp(line, "e") == 0 || strcmp(line, "ping") == 0) {
    runEchoTest(true);
    return;
  }
  if (strcmp(line, "tx") == 0) {
    runEchoTest(false);
    return;
  }
  if (strcmp(line, "loop") == 0) {
    printLoopHelp();
    return;
  }

  uint32_t baud = 0;
  if (parseBaudToken(line, &baud)) {
    applyBaud(baud);
    return;
  }

  Serial.printf("Unknown: '%s' — type help\n", line);
}

static void pollUsbCommands() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      cmdBuf[cmdLen] = '\0';
      handleCommand(cmdBuf);
      cmdLen = 0;
      continue;
    }
    if (cmdLen + 1 < sizeof(cmdBuf)) {
      cmdBuf[cmdLen++] = (char)c;
    } else {
      cmdLen = 0;
    }
  }
}

static void pollAutoEcho() {
  if (!autoEchoEnabled) return;
  if (autoEchoDone >= AUTO_ECHO_COUNT) return;
  uint32_t due = (uint32_t)autoEchoDone * AUTO_ECHO_INTERVAL_MS;
  if ((millis() - autoEchoStartMs) < due) return;

  uint8_t n = (uint8_t)(autoEchoDone + 1);
  Serial.println();
  Serial.printf("=== Auto echo %u/%u (bez klavesnice) ===\n",
                (unsigned)n, (unsigned)AUTO_ECHO_COUNT);
  runEchoTest(true);
  autoEchoDone = n;
}

static void pollBootButton() {
  if (!BOOT_BTN_RUNTIME_START) return;
  bool high = digitalRead(BOOT_BTN_PIN) != LOW;
  if (bootBtnPrev && !high) {
    delay(30);
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
      Serial.println();
      Serial.println(F("=== BOOT (GPIO0) — START_PROBE ==="));
      runStartProbe();
      while (digitalRead(BOOT_BTN_PIN) == LOW) {
        delay(10);
      }
      high = true;
    }
  }
  bootBtnPrev = high;
}

void setup() {
  Serial.begin(USB_BAUD);
  unsigned long start = millis();
  while (!Serial && (millis() - start < CDC_WAIT_MS)) {
    delay(10);
  }

  // GPIO0 = LCD G3 na 7B; INPUT_PULLUP jen kdyz by runtime BOOT byl zapnuty
  if (BOOT_BTN_RUNTIME_START) {
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    bootBtnPrev = digitalRead(BOOT_BTN_PIN) != LOW;
  }

  Serial.println();
  Serial.println(F("=== Spawer RS485 sniffer + Modbus + LCD test UI (H1/H100/H101/ERR) ==="));
  Serial.printf("Board: Waveshare ESP32-S3-Touch-LCD-7B (USB CDC)\n");
  Serial.printf("Bus UART: RX=GPIO%d TX=GPIO%d (auto DE/RE) @ %lu 8N1\n",
                RS485_RX_PIN, RS485_TX_PIN, (unsigned long)busBaud);
  Serial.println(F("Cil: H100 Start + ERR hunt (mimo 500-506) + LCD Start FC16."));
  Serial.println(F("Quiet Serial: H100= H1= H101= Hnnn= ERR_ADDR= + busy/ex6 / H100_START."));
  Serial.println(F("Fast ~1s: FC03 100-101, H1, 500-520. H0 OFF."));
  Serial.println(F("  ERR_DUMP po FC16 / H100 1→0 / errdump (kandidati + sparse)."));
  Serial.println(F("  Milan: panel Start → H100 CHANGE 1→0. LCD/h100/fc16 → FC16 + confirm."));
  Serial.println(F("Start = FC16 only (FC06 na teto pumpe nic nedela)."));
  Serial.println(F("Neni prepínac — napis: fc16   nebo  h100alt"));
  Serial.println(F("Auto TEMP scan OFF (nehladovi H100). Ručne: poll + verbose + errdump."));
  Serial.println(F("  FC04/FC01/FC02 OFF. START_PROBE auto OFF (jen 'start')."));
  Serial.println(F("  BOOT 7B = download/USB-reset (GPIO0=LCD G3). Serial zustava."));
  Serial.println(F("Sleduj: H100= ERR_ADDR= Hnnn= / CHANGE / H100_START TX|RX. TO → A<->B."));
  Serial.println(F("REFLASH: pio run -t upload; pak pio device monitor. Auto echo OFF."));
  printHelp();
  Serial.println();

  if (!uiTestInit()) {
    Serial.println(F("[UI] init failed — pokracuji jen Serial/Modbus."));
  }

  Serial1.begin(busBaud, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  lastActivityMs = millis();
  lastByteUs = micros();
  lastAliveMs = millis();
  autoEchoStartMs = millis();
  autoEchoDone = AUTO_ECHO_COUNT;  // Spawer: no auto echo unless echoon
  autoEchoEnabled = false;
  mbPollEnabled = true;
  mbVerbose = false;
  mbScanBusy = false;
  mbErrDumpBusy = false;
  mbLastErrDumpMs = 0;
  haveH0 = false;
  haveH1 = false;
  haveH100 = false;
  haveH101 = false;
  haveH500 = false;
  lastH0 = 0;
  lastH1 = 0;
  lastH100 = 0;
  lastH101 = 0;
  for (uint16_t i = 0; i < MB_H500_QTY; i++) lastH500[i] = 0;
  havePrevScan = false;
  prevCandN = 0;
  mbNextFastMs = millis() + 300;  // fast H100 watch first
  mbNextPollMs = millis() + MB_POLL_INTERVAL_MS;
  // Auto FC06 vypnuto — ruční start zůstává
  autoStartProbeDone = !AUTO_START_PROBE_ENABLED;
  autoStartProbeAtMs = millis() + AUTO_START_PROBE_MS;
}

void loop() {
  // Tick early so GT911 → Start CLICKED is not behind Modbus
  uiTestTick();
  pollUsbCommands();
  pollAutoEcho();
  pollBootButton();
  pollAutoStartProbe();
  if (uiTestConsumeStartRequest()) {
    runH100StartWrite(true);
  }
  pollModbusMaster();  // pumps uiTestTick inside RX waits
  uiTestTick();

  while (Serial1.available()) {
    int b = Serial1.read();
    if (b < 0) break;
    lastByteUs = micros();
    lastActivityMs = millis();
    silentAnnounced = false;

    if (buflen < MAX_FRAME) {
      buf[buflen++] = (uint8_t)b;
    } else {
      emitFrame(buf, buflen);
      buflen = 0;
      buf[buflen++] = (uint8_t)b;
    }
  }

  if (buflen > 0 && (micros() - lastByteUs) > frameGapUs()) {
    emitFrame(buf, buflen);
    buflen = 0;
  }

  if (!silentAnnounced && (millis() - lastActivityMs) > SILENT_MS) {
    counts[3]++;
    Serial.printf("[%lu ms] SILENT — no bytes for %lu ms "
                  "(check A/B, power, baud; type 4800 or 19200; "
                  "Modbus TIMEOUT? prohod A<->B)\n",
                  (unsigned long)millis(), (unsigned long)SILENT_MS);
    silentAnnounced = true;
  }

  if ((millis() - lastAliveMs) >= ALIVE_MS) {
    lastAliveMs = millis();
    Serial.printf("[%lu ms] alive — bus@%lu poll=%s",
                  (unsigned long)millis(), (unsigned long)busBaud,
                  mbPollEnabled ? "on" : "off");
    if (haveH1) Serial.printf(" H1=%u", (unsigned)lastH1);
    else Serial.print(F(" H1=?"));
    if (haveH100) Serial.printf(" H100=%u", (unsigned)lastH100);
    else Serial.print(F(" H100=?"));
    if (haveH101) Serial.printf(" H101=%u", (unsigned)lastH101);
    else Serial.print(F(" H101=?"));
    Serial.printf(" OK=%lu TO=%lu MODBUS=%lu PROP80=%lu OTHER=%lu SILENT_ev=%lu\n",
                  (unsigned long)mbOkCount, (unsigned long)mbTimeoutCount,
                  (unsigned long)counts[0], (unsigned long)counts[1],
                  (unsigned long)counts[2], (unsigned long)counts[3]);
  }
}
