#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Install the Canon LBP-810 CAPT Print Service on Windows 10 x64.

.DESCRIPTION
    This script:
    1. Installs the CAPT background service (capt-service.exe)
    2. Starts the service (IPP server on localhost:6631)
    3. Adds a Windows printer using Microsoft's built-in IPP Class Driver
    4. Points the printer to the local IPP endpoint

    Prerequisites:
    - The Canon LBP-810 must have its USB driver switched from usbprint.sys
      to winusb.sys using the provided capt-lbp810.inf file or Zadig.
    - Run this script as Administrator.

.PARAMETER ServicePath
    Path to capt-service.exe. Defaults to ..\build\capt-service.exe relative to script.

.PARAMETER ServiceOnly
    Install or update only the background Windows service without touching any printer.

.PARAMETER Uninstall
    Remove the printer and service.
#>
param(
    [string]$ServicePath = "$PSScriptRoot\..\build\capt-service.exe",
    [switch]$ServiceOnly,
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

$ServiceName  = "CaptLBP810"
$PrinterName  = "Canon LBP-810"
$PortName     = "IPP_CAPT_LBP810"
$PortAddress  = "127.0.0.1"
$PortNumber   = 6631
$PrinterURL   = "http://127.0.0.1:$PortNumber/printers/canon-lbp810"
$DriverName   = "Microsoft IPP Class Driver"
$LogDir       = "$env:ProgramData\CaptLBP810"

# ─── Uninstall ───────────────────────────────────────────
if ($Uninstall) {
    Write-Host "=== Uninstalling Canon LBP-810 CAPT ===" -ForegroundColor Yellow

    # Remove printer
    $printer = Get-Printer -Name $PrinterName -ErrorAction SilentlyContinue
    if ($printer) {
        Remove-Printer -Name $PrinterName
        Write-Host "  Removed printer '$PrinterName'"
    }

    # Remove port
    $port = Get-PrinterPort -Name $PortName -ErrorAction SilentlyContinue
    if ($port) {
        Remove-PrinterPort -Name $PortName
        Write-Host "  Removed printer port '$PortName'"
    }

    # Stop and remove service
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($svc) {
        if ($svc.Status -eq 'Running') {
            Stop-Service $ServiceName -Force
            Write-Host "  Stopped service"
        }
        sc.exe delete $ServiceName | Out-Null
        Write-Host "  Removed service"
    }

    Write-Host "`nUninstallation complete." -ForegroundColor Green
    exit 0
}

# ─── Install ─────────────────────────────────────────────
Write-Host "=== Installing Canon LBP-810 CAPT Print Service ===" -ForegroundColor Cyan
Write-Host ""

# Verify service executable exists
$ServicePath = (Resolve-Path $ServicePath -ErrorAction SilentlyContinue).Path
if (-not $ServicePath -or -not (Test-Path $ServicePath)) {
    Write-Error "capt-service.exe not found at '$ServicePath'. Build the project first."
    exit 1
}
Write-Host "  Service binary: $ServicePath"

# Step 1: Checking and configuring USB device driver (Roadmap Phase 1.1)
Write-Host ""
Write-Host "Step 1: Checking and configuring USB device driver..." -ForegroundColor White
$infPath = Join-Path $PSScriptRoot "capt-lbp810.inf"
$usbDevice = Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like "*VID_04A9&PID_260A*"
} | Select-Object -First 1

if (-not $usbDevice) {
    Write-Warning "Canon LBP-810 USB device not currently plugged in."
    if (Test-Path $infPath) {
        Write-Host "  Pre-staging capt-lbp810.inf to Windows Driver Store via pnputil..."
        & pnputil.exe /add-driver "$infPath" | Out-Null
        Write-Host "  Driver pre-staged successfully. Windows will automatically bind WinUSB when connected." -ForegroundColor Green
    }
} else {
    Write-Host "  Found: $($usbDevice.FriendlyName) ($($usbDevice.InstanceId))"
    if ($usbDevice.Service -eq "WinUSB") {
        Write-Host "  Driver: WinUSB (correct!)" -ForegroundColor Green
    } else {
        Write-Host "  Current driver: $($usbDevice.Service). Attempting automated WinUSB driver binding..." -ForegroundColor Yellow
        if (Test-Path $infPath) {
            $pnpResult = & pnputil.exe /add-driver "$infPath" /install
            Start-Sleep -Seconds 2
            # Re-check device service
            $recheck = Get-PnpDevice -PresentOnly | Where-Object {
                $_.InstanceId -like "*VID_04A9&PID_260A*"
            } | Select-Object -First 1
            if ($recheck -and $recheck.Service -eq "WinUSB") {
                Write-Host "  Successfully switched driver to WinUSB via PnPUtil!" -ForegroundColor Green
            } else {
                Write-Warning "  PnPUtil could not automatically replace $($usbDevice.Service) with WinUSB."
                Write-Host "  You may manually install the driver:"
                Write-Host "    Option A: Device Manager -> Update Driver -> Browse -> $infPath"
                Write-Host "    Option B: Use Zadig (https://zadig.akeo.ie/) to replace with WinUSB"
            }
        }
    }
}

# Step 2: Create log directory
Write-Host ""
Write-Host "Step 2: Creating log directory..." -ForegroundColor White
if (-not (Test-Path $LogDir)) {
    New-Item -Path $LogDir -ItemType Directory -Force | Out-Null
}
Write-Host "  Log directory: $LogDir"

# Step 3: Install Windows service
Write-Host ""
Write-Host "Step 3: Installing CAPT service..." -ForegroundColor White
$existing = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($existing) {
    if ($existing.Status -eq 'Running') {
        Stop-Service $ServiceName -Force
        Start-Sleep 1
    }
    sc.exe delete $ServiceName | Out-Null
    Start-Sleep 2
    Write-Host "  Removed existing service"
}

New-Service -Name $ServiceName `
            -BinaryPathName "`"$ServicePath`"" `
            -DisplayName "Canon LBP-810 CAPT Print Service" `
            -Description "Receives IPP print jobs and translates to Canon CAPT protocol for the LBP-810 laser printer." `
            -StartupType Automatic | Out-Null

# Set failure recovery: restart on first and second failure
sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/10000// | Out-Null

Start-Service $ServiceName
Write-Host "  Service installed and started" -ForegroundColor Green

# Step 4: Wait for IPP server to be ready
Write-Host ""
Write-Host "Step 4: Waiting for IPP server to start..." -ForegroundColor White
$ready = $false
for ($i = 0; $i -lt 15; $i++) {
    Start-Sleep 1
    try {
        $tcp = New-Object System.Net.Sockets.TcpClient
        $tcp.Connect("127.0.0.1", $PortNumber)
        $tcp.Close()
        $ready = $true
        break
    } catch {
        Write-Host "  Waiting... ($($i+1)/15)"
    }
}

if (-not $ready) {
    Write-Warning "  IPP server did not start within 15 seconds."
    Write-Host "  Check the log file at: $LogDir\capt-service.log"
    Write-Host "  You can also run 'capt-service.exe --console' for debugging."
} else {
    Write-Host "  IPP server is ready on port $PortNumber" -ForegroundColor Green
}

# Step 5: Add printer via IPP URL (if not already installed or if -ServiceOnly not specified)
if ($ServiceOnly) {
    Write-Host ""
    Write-Host "Step 5: Skipping printer configuration (-ServiceOnly specified)." -ForegroundColor Yellow
} else {
    Write-Host ""
    Write-Host "Step 5: Checking Windows printer configuration..." -ForegroundColor White

    # Check if a printer already exists with this name or pointing to our IPP port
    $existingPrinter = Get-Printer -Name $PrinterName -ErrorAction SilentlyContinue
    if (-not $existingPrinter) {
        $existingPrinter = Get-Printer -ErrorAction SilentlyContinue | Where-Object {
            $_.PortName -like "*$PortNumber*" -or $_.Name -like "*LBP*810*" -or $_.PortName -like "*canon-lbp810*"
        } | Select-Object -First 1
    }

    $TargetPrinterName = $PrinterName
    if ($existingPrinter) {
        $TargetPrinterName = $existingPrinter.Name
        Write-Host "  Printer already configured: '$TargetPrinterName' (Port: $($existingPrinter.PortName))" -ForegroundColor Green
        Write-Host "  Preserving existing working printer."
    } else {
        # Remove stale TCP port if leftover from previous installs
        $oldPort = Get-PrinterPort -Name $PortName -ErrorAction SilentlyContinue
        if ($oldPort) {
            Remove-PrinterPort -Name $PortName -ErrorAction SilentlyContinue
            Write-Host "  Removed stale TCP port '$PortName'"
        }

        # Add the printer using its IPP URL. This tells Windows to use the
        # Microsoft IPP Class Driver and speak HTTP/IPP to our service.
        $IppUrl = "http://localhost:$PortNumber/printers/canon-lbp810"

        try {
            Write-Host "  Adding printer via native IPP (Add-Printer -IppUrl)..."
            Add-Printer -Name $PrinterName -IppUrl $IppUrl -ErrorAction Stop
            Write-Host "  Printer '$PrinterName' added successfully via IPP!" -ForegroundColor Green
            Write-Host "  URL: $IppUrl"
        } catch {
            Write-Warning "  Native Add-Printer -IppUrl failed: $_"
            Write-Host "  Attempting fallback via printui.dll..."
            try {
                $proc = Start-Process -FilePath "rundll32.exe" `
                    -ArgumentList "printui.dll,PrintUIEntry /if /b `"$PrinterName`" /r `"$IppUrl`" /m `"$DriverName`" /q" `
                    -Wait -PassThru -NoNewWindow
                
                if ($proc.ExitCode -eq 0) {
                    Write-Host "  Printer '$PrinterName' added via IPP!" -ForegroundColor Green
                    Write-Host "  URL: $IppUrl"
                } else {
                    throw "printui returned exit code $($proc.ExitCode)"
                }
            } catch {
                Write-Warning "  Automatic printer setup failed: $_"
                Write-Host ""
                Write-Host "  Please verify the printer is added in Windows Settings -> Printers & scanners" -ForegroundColor Yellow
                Write-Host "  using IPP URL: $IppUrl" -ForegroundColor Cyan
                Write-Host ""
            }
        }
    }

    # Set default paper size to A4 in Windows printer preferences
    try {
        Set-PrintConfiguration -PrinterName $TargetPrinterName -PaperSize A4 -ErrorAction SilentlyContinue
        Write-Host "  Set default paper size to A4 for '$TargetPrinterName'" -ForegroundColor Green
    } catch {
        # Optional setting, continue if unsupported on older OS
    }
}

# Done
Write-Host ""
Write-Host "=" * 50 -ForegroundColor Cyan
Write-Host "  Installation Complete!" -ForegroundColor Green
Write-Host "=" * 50 -ForegroundColor Cyan
Write-Host ""
Write-Host "  Printer: $PrinterName"
Write-Host "  Service: $ServiceName (running)"
Write-Host "  Log:     $LogDir\capt-service.log"
Write-Host ""
Write-Host "  To test: Print a test page from Settings -> Printers"
Write-Host "  To uninstall: .\install.ps1 -Uninstall"
Write-Host ""
