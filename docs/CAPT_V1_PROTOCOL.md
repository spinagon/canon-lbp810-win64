# Canon Advanced Printing Technology (CAPT) v1 Protocol Specification

This document provides a formal technical specification of the CAPT v1 communication protocol utilized by the **Canon LBP-810** laser printer over USB.

---

## 1. USB Endpoint Topology & Hardware Interface

The Canon LBP-810 exposes a vendor-specific USB device interface (VID `0x04A9`, PID `0x260A`).

| Endpoint | Direction | Transfer Type | Max Packet Size | Description |
| :--- | :--- | :--- | :--- | :--- |
| **EP 0** | Bidirectional | Control | 64 bytes | Standard USB device descriptors and enumeration |
| **EP 1 (0x81)** | IN (Device &rarr; Host) | Bulk | 64 bytes | Status queries, identification, command responses |
| **EP 2 (0x02)** | OUT (Host &rarr; Device) | Bulk | 64 bytes | Command packets, configuration payloads, compressed raster streams |

On 64-bit Windows, standard kernel driver binding is achieved via Microsoft's generic `winusb.sys` (WinUSB).

---

## 2. Packet Framing Architecture

All command transmissions and responses across EP2 / EP1 follow a 4-byte fixed header framing protocol.

### 2.1 Host-to-Device (Bulk OUT) Header

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Opcode (16-bit LE)      |   Packet Length (16-bit LE)   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  Optional Payload Data ...                    |
```

- **Bytes 0–1 (Opcode)**: 16-bit integer in Little-Endian byte order. Example: `0xD0A0` is sent as byte `0xA0`, then byte `0xD0`.
- **Bytes 2–3 (Total Packet Length)**: 16-bit integer in Little-Endian byte order representing total packet length including the 4-byte header (`4 + payload_len`).
  - No payload (`0` bytes) &rarr; `0x04`, `0x00` (4 bytes total)
  - `8` bytes payload &rarr; `0x0C`, `0x00` (12 bytes total)
  - `34` bytes payload &rarr; `0x26`, `0x00` (38 bytes total)
  - `4096` bytes payload &rarr; `0x04`, `0x10` (4100 bytes total)

### 2.2 Device-to-Host (Bulk IN) Response Header

Bulk IN responses mirror the opcode in bytes 0–1 (LE), followed by a 2-byte size in bytes 2–3 indicating the response length. Depending on the command response, certain engine status responses report this length in 4-digit BCD format (handled via `bcd_decode`).

---

## 3. Protocol Opcodes & Payload Definitions

| Opcode | Hex Code | Direction | Payload Size | Purpose / Semantics |
| :--- | :---: | :---: | :---: | :--- |
| `CAPT_IEEE_IDENT` | `0xA1A0` | OUT &rarr; IN | 0 bytes OUT, ~128B IN | Retrieves standard IEEE-1284 1284 device ID string |
| `CAPT_GET_PRINTER_INFO` | `0xA1A1` | OUT &rarr; IN | 0 bytes OUT, 4–64B IN | Queries engine model info, firmware, and block transfer size |
| `CAPT_RESERVE_UNIT` | `0xA2A0` | OUT &rarr; IN | 0 bytes OUT, 4B IN | Locks printer exclusive access for the print job |
| `CAPT_RELEASE_UNIT` | `0xE0A9` | OUT &rarr; IN | 0 bytes OUT, 4B IN | Releases unit reservation at the conclusion of a job |
| `CAPT_GET_EXTENDED_STATUS`| `0xA0A0` | OUT &rarr; IN | 0 bytes OUT, 16–256B IN| Fetches full 16-byte extended engine hardware status vector |
| `CAPT_GET_BASIC_STATUS` | `0xE0A0` | OUT &rarr; IN | 0 bytes OUT, 1B IN | Fast 1-byte status query (`0x08` = buffer full, `0x02` = busy) |
| `CAPT_GO_ONLINE` | `0xE0A5` | OUT &rarr; IN | 8 bytes OUT, 4B IN | Wakes engine and arms laser pickup for the specified page |
| `CAPT_GO_OFFLINE` | `0xE0A6` | OUT | 0 bytes | Abruptly puts printer offline (not recommended mid-spool) |
| `CAPT_BEGIN_PAGE` | `0xD0A0` | OUT | 34 bytes OUT | Configures paper geometry, margins, resolution, quality |
| `CAPT_BEGIN_DATA` | `0xD0A1` | OUT | 0 bytes | Signals beginning of compressed SCoA raster transmission |
| `CAPT_PRINT_DATA` | `0xC0A0` | OUT | Variable (up to block) | Transmits raw SCoA compressed raster data chunks |
| `CAPT_END_PAGE` | `0xD0A2` | OUT | 0 bytes | Finalizes page raster transmission, commands engine ejection |
| `CAPT_CLEAR_ERROR` | `0xE0A2` | OUT &rarr; IN | 0 bytes OUT, 4B IN | Clears non-fatal communication or controller error state |
| `CAPT_CLEAR_MISPRINT` | `0xE0A3` | OUT &rarr; IN | 0 bytes OUT, 4B IN | Clears engine misprint flag after paper feed restart |
| `CAPT_DISCARD_DATA` | `0xE0A4` | OUT &rarr; IN | 0 bytes OUT, 4B IN | Flushes unprinted raster chunks from engine buffer (Cancel) |
| `CAPT_RESET_ENGINE` | `0xE0A1` | OUT &rarr; IN | 0 bytes OUT, 4B IN | Resets engine micro-controller state machine |

---

## 4. Page Parameters Payload (`0xD0A0` - 34 Bytes)

The `CAPT_BEGIN_PAGE` (`0xD0A0`) packet requires an exact 34-byte binary payload:

```
Offset  Size  Field                Description
──────────────────────────────────────────────────────────────────
0x00    2     reserved             Always 0x0000
0x02    2     target_model         Model identifier: 0x01A4 (Canon LBP-810)
0x04    1     paper_size           Paper size opcode (see Table below)
0x05    1     media_source         Media source: 0x01 (auto cassette) or 0x00 (manual slot)
0x06    1     input_slot           Hardware cassette index (always 0x00 for LBP-810)
0x07    1     reserved             Always 0x00
0x08    4     toner_density        4 bytes toner density (0x1F, 0x1F, 0x1F, 0x1F max)
0x0C    1     mode                 Engine operating mode: 0x00
0x0D    1     resolution           Resolution code: 0x11 (600 DPI), 0x00 (300 DPI)
0x0E    4     constants            Engine tuning constants: 0x03, 0x01, 0x01, 0x01
0x12    1     smoothing            Smoothing / AIR: 0x02 (ON), 0x00 (OFF)
0x13    1     toner_saving         Toner saving mode: 0x01 (draft), 0x00 (normal)
0x14    2     reserved             Always 0x0000
0x16    2     margin_left          Left margin offset in pixels (LE uint16)
0x18    2     margin_top           Top margin offset in pixels (LE uint16)
0x1A    2     image_line_size      Printable scanline width in bytes (LE uint16)
0x1C    2     image_lines          Printable scanline count (LE uint16)
0x1E    2     paper_width          Full physical sheet width in pixels (LE uint16)
0x20    2     paper_height         Full physical sheet height in pixels (LE uint16)
```

### Supported Paper Sizes

| Format | Code | 600 DPI Dimensions | 600 DPI Margins (L, T) | Printable Size | Line Bytes |
| :--- | :---: | :--- | :--- | :---: | :---: |
| **ISO A4** | `0x02` | 4960 × 7014 | (112, 119) | 4736 × 6776 | 592 |
| **ISO A5** | `0x03` | 3496 × 4960 | (100, 110) | 3296 × 4740 | 412 |
| **Envelope COM10**| `0x05` | 2475 × 5700 | (100, 120) | 2275 × 5460 | 285 |
| **JIS B5** | `0x07` | 4299 × 6070 | (100, 110) | 4099 × 5850 | 513 |
| **Envelope C5** | `0x08` | 3826 × 5409 | (100, 110) | 3626 × 5189 | 454 |
| **Executive** | `0x0A` | 4350 × 6300 | (100, 120) | 4150 × 6060 | 520 |
| **Envelope DL** | `0x0B` | 2598 × 5196 | (100, 110) | 2398 × 4976 | 300 |
| **US Legal** | `0x0C` | 5100 × 8400 | (110, 120) | 4880 × 8160 | 610 |
| **US Letter** | `0x0D` | 5100 × 6600 | (110, 120) | 4880 × 6360 | 610 |

---

## 5. SCoA (Smart Compression Architecture) Compression

SCoA is Canon's proprietary run-length and delta-differencing compression format used for bi-level (1bpp) raster streaming.

### 5.1 Opcodes & Control Codes

- **Literal Run** (`0x00 .. 0x7F`): Emits literal raw bytes directly.
- **Repeat Pattern** (`0x80 .. 0xBF`): Emits repeated byte patterns across scanlines.
- **Delta Copy** (`0xC0 .. 0xDF`): References unchanged bytes from the preceding scanline.
- **End of Page (EOP)** (`0xFD` or `0xFF` sequence): Signals conclusion of page raster stream.

---

## 6. Engine Status Decoding & Error Mapping

Extended status responses (`0xA0A0`) yield a minimum of 16 status bytes:

| Byte | Field | Bitmask | Interpretation |
| :---: | :--- | :--- | :--- |
| `0` | `basic` | `0x02` | `NOT_READY` (1 if engine is not ready) |
| `0` | `basic` | `0x08` | `IM_DATA_BUSY` (1 if onboard FIFO buffer is full) |
| `0` | `basic` | `0x80` | `GENERAL_ERROR` (active fatal error) |
| `2` | `aux` | `0x06` | Feed roller / engine motion active |
| `6–7` | `engine` | `0x0100` | Paper Jam (`media-jam-error`, when `basic & 0x80` active) |
| `6–7` | `engine` | `0x0200` | No Paper in Tray (`media-empty-error`, when `basic & 0x80` active) |
| `6–7` | `engine` | `0x2000` | No Toner Cartridge (`marker-supply-missing-error`) |
| `6–7` | `engine` | `0x4000` | Front Door / Cover Open (`door-open-error`) |
| `14–15`| `page_printed`| uint16 | Incremented upon physical sheet ejection to output tray |
