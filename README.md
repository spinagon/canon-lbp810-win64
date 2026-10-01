# Canon LBP-810 CAPT Print Service for Windows 10 x64

A complete, persistent printing solution for the Canon LBP-810 laser printer on Windows 10/11 64-bit, where no official 64-bit driver exists.

## How It Works

Instead of an unsigned kernel-mode printer driver (which cannot run on modern 64-bit Windows), this project runs a lightweight background Windows service that:

1. **IPP Server** — Listens on `http://127.0.0.1:6631/printers/canon-lbp810` as a standard IPP 2.0 printer. (Port 6631 is used to avoid port 631 conflicts with Windows Internet Printing).
2. **Microsoft IPP Class Driver** — Windows connects using its built-in driver with zero driver signing or disabling driver signature enforcement required.
3. **PWG-Raster Decoding & Dithering** — Receives standard PWG-Raster data from Windows, decodes PackBits with line repetition per PWG 5102.4, and dithers 8bpp grayscale to 1bpp monochrome via Floyd-Steinberg error diffusion.
4. **CAPT v1 Protocol Translation** — Compresses monochrome bitmaps using SCoA delta-compression and frames them into Canon CAPT v1 USB packets.
5. **Direct USB Communication** — Communicates directly with the printer hardware via WinUSB (`winusb.sys`).

```
Windows App → Print Spooler → Microsoft IPP Class Driver
    → http://localhost:6631/printers/canon-lbp810 → capt-service.exe → WinUSB → Canon LBP-810
```

## Requirements

- Windows 10 x64 (or Windows 11 x64)
- Canon LBP-810 connected via USB (`VID 04A9, PID 260A`)
- Administrator access (for driver assignment and service installation)

## Installation

### Step 1: Switch USB Driver to WinUSB

The printer's USB interface must be handled by `winusb.sys` instead of the default `usbprint.sys`.

**Option A: Zadig (Recommended / Easiest)**
1. Connect the printer via USB and turn it on.
2. Download and run [Zadig](https://zadig.akeo.ie/).
3. In the menu, go to **Options** → check **List All Devices**.
4. Select **Canon CAPT USB Device** or **Canon LBP-810** (`USB ID: 04A9 260A`).
5. Select **WinUSB** as the replacement driver and click **Replace Driver** (or **Install Driver**).

**Option B: Device Manager**
1. Open Device Manager (`devmgmt.msc`).
2. Locate the printer under **Printers**, **Universal Serial Bus devices**, or **Other devices**.
3. Right-click the printer → **Update driver** → **Browse my computer for drivers**.
4. Select **Let me pick from a list of available drivers on my computer** → **Have Disk...**.
5. Browse to and select `install\capt-lbp810.inf`, then confirm the installation.
*(Alternatively, run from an Administrator prompt: `pnputil /add-driver install\capt-lbp810.inf /install`)*

---

### Step 2: Install the Service

Run PowerShell as Administrator:

```powershell
cd install
.\install.ps1
```

**Script Options & Parameters:**
* `.\install.ps1` — Full automated installation: installs the background service and configures the Windows IPP printer. If the printer is already configured in Windows, it safely detects and preserves it without changes.
* `.\install.ps1 -ServiceOnly` — Installs/restarts only the background Windows service without touching existing printer devices.
* `.\install.ps1 -ServicePath "C:\path\to\capt-service.exe"` — Specifies a custom location for the executable (useful when compiled with Visual Studio in `build\Release\`).
* `.\install.ps1 -Uninstall` — Stops and removes the service, printer, and port.

**Service Details:**
* **Service Name**: `CaptLBP810`
* **Display Name**: `Canon LBP-810 CAPT Print Service`
* **Startup Type**: Automatic (starts quietly on Windows boot)
* **Failure Recovery**: Automatically restarts after 5s/10s on failure
* **Log File**: `C:\ProgramData\CaptLBP810\capt-service.log`

---

### Step 3: Print!

The printer is ready as **"Canon LBP-810"** in **Settings → Bluetooth & devices → Printers & scanners**. Print from any Windows application (Word, Acrobat, Notepad, web browsers) as normal.

---

## Console Mode (Debugging & Diagnostics)

To test the printer or observe real-time status and protocol packet exchanges in the foreground:

```cmd
# Stop the background service first if running:
sc stop CaptLBP810

# Run in foreground console mode:
capt-service.exe --console

# Custom port if desired (default: 6631):
capt-service.exe --console --port 6631
```

To restart the background service when finished debugging:
```cmd
sc start CaptLBP810
```

---

## Building from Source

### On Linux (Cross-Compile for Windows x64)

```bash
# Install MinGW-w64
sudo apt install mingw-w64 cmake make

# Cross-compile for Windows
cmake -B build-win -S . -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-win -j$(nproc)
```
Output executable: `build-win/capt-service.exe`

### On Linux (Native, for Unit Testing)

```bash
# Configure and build native tests
cmake -B build -S .
cmake --build build -j$(nproc)

# Run test suite
ctest --test-dir build --output-on-failure
```

### On Windows (Visual Studio)

In a Visual Studio Developer Command Prompt or PowerShell:

```cmd
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
Output executable: `build\Release\capt-service.exe`

---

## Supported Features

| Feature | Support |
|---------|---------|
| Paper Sizes | ISO A4 (`210x297mm`), US Letter (`8.5x11in`), auto-detected per job |
| Input Tray / Source | Main Auto Cassette (`0x01`) |
| Resolutions | 600 DPI (default), 300 DPI |
| Raster Decoding | Standard PWG-Raster (PWG 5102.4) with PackBits line repetition |
| Dithering | Floyd-Steinberg error diffusion (8bpp sGray → 1bpp mono) |
| Compression | SCoA delta byte-stream compression |
| Multi-Page Jobs | Full multi-page support with clean page ejection |
| Status Monitoring | Tray empty / paper out, door/cover open, paper jam, no cartridge |
| Error Recovery | Print engine reset (`0xE0A1`) & misprint clearing on job start |

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    capt-service.exe                     │
│                                                         │
│  ┌──────────────┐   ┌────────────────┐   ┌────────────┐ │
│  │  IPP Server  │──▶│   PWG-Raster   │──▶│  Ditherer  │ │
│  │ (port 6631)  │   │  PackBits+Rep  │   │   (F-S)    │ │
│  └──────────────┘   └────────────────┘   └─────┬──────┘ │
│                                                │        │
│  ┌──────────────┐   ┌────────────────┐   ┌─────▼──────┐ │
│  │    WinUSB    │◀──│  CAPT v1 Engine│◀──│    SCoA    │ │
│  │  (USB Bulk)  │   │   State Mach.  │   │ Compressor │ │
│  └──────┬───────┘   └────────────────┘   └────────────┘ │
└─────────┼───────────────────────────────────────────────┘
          │ USB Bulk IN (0x81) / OUT (0x02)
          ▼
    ┌───────────┐
    │  LBP-810  │
    └───────────┘
```

---

## Roadmap

For the engineering roadmap toward full hardware capability coverage, automated single-click installer packaging, and production-grade tooling, see [ROADMAP.md](ROADMAP.md).

---

## Credits

CAPT protocol reverse-engineering references:
- Nicolas Boichat (original `capt-0.1`, 2004)
- `darkvision77/libcapt` (SCoA compression implementation)
- `agalakhov/captdriver` (protocol specifications)
- `mounaiban/captdriver` (extended status handling & engine commands)

---

## License

This project is released under the MIT License.
