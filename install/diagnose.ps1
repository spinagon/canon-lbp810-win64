<#
.SYNOPSIS
    One-Click Diagnostics Tool for Canon LBP-810 CAPT Service
.DESCRIPTION
    Performs 6 comprehensive health checks on USB hardware, WinUSB binding,
    Windows Service status, IPP port 6631 connectivity, Spooler queue, and logs.
#>

$ErrorActionPreference = "Continue"

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "   Canon LBP-810 CAPT Service — System Diagnostics        " -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host ""

$issuesFound = 0
$warningsFound = 0

# ── Check 1: USB Hardware Presence ───────────────────────
Write-Host "[1/6] Checking USB Hardware Presence (VID_04A9&PID_260A)..." -NoNewline
$usbDev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object {
    $_.InstanceId -like "*VID_04A9&PID_260A*"
} | Select-Object -First 1

if ($usbDev) {
    Write-Host " [PASS]" -ForegroundColor Green
    Write-Host "      Device: $($usbDev.FriendlyName) ($($usbDev.InstanceId))" -ForegroundColor Gray
} else {
    Write-Host " [WARN]" -ForegroundColor Yellow
    Write-Host "      Canon LBP-810 USB device was not detected in active PnP tree." -ForegroundColor Yellow
    Write-Host "      Ensure USB cable is firmly plugged in and printer is powered on." -ForegroundColor Gray
    $warningsFound++
}

# ── Check 2: USB Driver Verification ─────────────────────
Write-Host "[2/6] Checking USB Driver Binding..." -NoNewline
if ($usbDev) {
    if ($usbDev.Service -eq "WinUSB") {
        Write-Host " [PASS]" -ForegroundColor Green
        Write-Host "      Driver bound to WinUSB (correct)." -ForegroundColor Gray
    } elseif ($usbDev.Service -eq "usbprint") {
        Write-Host " [FAIL]" -ForegroundColor Red
        Write-Host "      Device is still using usbprint.sys! Must be bound to WinUSB." -ForegroundColor Red
        Write-Host "      Fix: Run 'pnputil.exe /add-driver install\capt-lbp810.inf /install' or use Zadig." -ForegroundColor Yellow
        $issuesFound++
    } else {
        Write-Host " [WARN]" -ForegroundColor Yellow
        Write-Host "      Current driver service: $($usbDev.Service)" -ForegroundColor Yellow
        $warningsFound++
    }
} else {
    Write-Host " [SKIP] (Printer not plugged in)" -ForegroundColor DarkGray
}

# ── Check 3: Windows Service Status ───────────────────────
Write-Host "[3/6] Checking Windows Service (CaptLBP810)..." -NoNewline
$svc = Get-Service -Name "CaptLBP810" -ErrorAction SilentlyContinue

if ($svc) {
    if ($svc.Status -eq "Running") {
        Write-Host " [PASS]" -ForegroundColor Green
        Write-Host "      Service is running." -ForegroundColor Gray
    } else {
        Write-Host " [FAIL]" -ForegroundColor Red
        Write-Host "      Service status is '$($svc.Status)'. Start it with: Start-Service CaptLBP810" -ForegroundColor Red
        $issuesFound++
    }
} else {
    Write-Host " [FAIL]" -ForegroundColor Red
    Write-Host "      Service 'CaptLBP810' is not installed." -ForegroundColor Red
    Write-Host "      Fix: Run install\install.ps1 as Administrator." -ForegroundColor Yellow
    $issuesFound++
}

# ── Check 4: TCP Loopback & IPP Connectivity ─────────────
Write-Host "[4/6] Checking TCP Loopback (127.0.0.1:6631)..." -NoNewline
$tcpClient = New-Object System.Net.Sockets.TcpClient
$portOpen = $false
try {
    $iar = $tcpClient.BeginConnect("127.0.0.1", 6631, $null, $null)
    $portOpen = $iar.AsyncWaitHandle.WaitOne(2000, $false)
    if ($portOpen) {
        $tcpClient.EndConnect($iar)
    }
} catch {
    $portOpen = $false
} finally {
    $tcpClient.Close()
}

if ($portOpen) {
    Write-Host " [PASS]" -ForegroundColor Green
    Write-Host "      Port 6631 is responding." -ForegroundColor Gray
} else {
    Write-Host " [FAIL]" -ForegroundColor Red
    Write-Host "      Port 6631 is not reachable. IPP server is not listening." -ForegroundColor Red
    $issuesFound++
}

# ── Check 5: Windows Spooler Queue Inspection ────────────
Write-Host "[5/6] Checking Windows Spooler Queue..." -NoNewline
$spoolerSvc = Get-Service -Name "Spooler" -ErrorAction SilentlyContinue
if (-not $spoolerSvc -or $spoolerSvc.Status -ne "Running") {
    Write-Host " [FAIL]" -ForegroundColor Red
    Write-Host "      Print Spooler service is NOT running." -ForegroundColor Red
    $issuesFound++
} else {
    $printer = Get-Printer -Name "Canon LBP-810" -ErrorAction SilentlyContinue
    if ($printer) {
        $jobs = Get-PrintJob -PrinterName "Canon LBP-810" -ErrorAction SilentlyContinue
        $stuckJobs = @($jobs | Where-Object { $_.JobStatus -match "Error|Blocked|UserIntervention" })
        if ($stuckJobs.Count -gt 0) {
            Write-Host " [WARN]" -ForegroundColor Yellow
            Write-Host "      Found $($stuckJobs.Count) stale or stuck print job(s) in queue." -ForegroundColor Yellow
            Write-Host "      Fix: Open Print Queue and cancel stuck jobs." -ForegroundColor Gray
            $warningsFound++
        } else {
            Write-Host " [PASS]" -ForegroundColor Green
            Write-Host "      Printer found, queue clean ($($jobs.Count) active jobs)." -ForegroundColor Gray
        }
    } else {
        Write-Host " [WARN]" -ForegroundColor Yellow
        Write-Host "      Printer 'Canon LBP-810' is not registered in Windows Spooler." -ForegroundColor Yellow
        Write-Host "      Fix: Run install\install.ps1 to configure the IPP printer." -ForegroundColor Gray
        $warningsFound++
    }
}

# ── Check 6: Service Log Inspection ───────────────────────
Write-Host "[6/6] Inspecting Service Log (last 25 lines)..." -ForegroundColor White
$logFile = "$env:ProgramData\CaptLBP810\capt-service.log"
if (Test-Path $logFile) {
    Write-Host "────────────────── Recent Log Entries ──────────────────" -ForegroundColor DarkGray
    Get-Content $logFile -Tail 25 | ForEach-Object {
        if ($_ -match "ERROR") {
            Write-Host "  $_" -ForegroundColor Red
        } elseif ($_ -match "WARN") {
            Write-Host "  $_" -ForegroundColor Yellow
        } else {
            Write-Host "  $_" -ForegroundColor Gray
        }
    }
    Write-Host "────────────────────────────────────────────────────────" -ForegroundColor DarkGray
} else {
    Write-Host "      No log file found at: $logFile" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "================ Diagnostics Summary ================" -ForegroundColor Cyan
if ($issuesFound -eq 0 -and $warningsFound -eq 0) {
    Write-Host "  All checks PASSED! System is completely operational." -ForegroundColor Green
} elseif ($issuesFound -eq 0) {
    Write-Host "  Checks passed with $warningsFound warning(s). Printer should work once plugged in." -ForegroundColor Yellow
} else {
    Write-Host "  Found $issuesFound critical issue(s) and $warningsFound warning(s)." -ForegroundColor Red
    Write-Host "  Please review the recommendations above." -ForegroundColor Red
}
Write-Host "=====================================================" -ForegroundColor Cyan
