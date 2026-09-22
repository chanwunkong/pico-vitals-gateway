<#
.SYNOPSIS
    Watch the Pico's USB CDC serial port, prefix each line with a timestamp,
    print it to screen, and also save it to a log file for later diffing
    with tools/compare_gm700sb_adv.py.

.DESCRIPTION
    PROJECT_PLAN.md notes that Windows' .NET SerialPort does not assert DTR
    by default, so the Pico's TinyUSB CDC will output nothing at all unless
    DtrEnable/RtsEnable are set after Open().

.PARAMETER Port
    The Pico's COM port (e.g. COM3). Check Device Manager, or
    Get-WmiObject Win32_SerialPort, if you don't know the number.

.PARAMETER OutFile
    Where to save the log. Defaults to a timestamped file under tools/.

.EXAMPLE
    powershell -File tools/capture_serial.ps1 -Port COM3
    powershell -File tools/capture_serial.ps1 -Port COM3 -OutFile idle.log
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Port,

    [string]$OutFile = "",

    [int]$BaudRate = 115200
)

if ([string]::IsNullOrWhiteSpace($OutFile)) {
    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutFile = Join-Path $PSScriptRoot "gm700sb_capture_$timestamp.log"
}

Write-Host "Opening $Port @ $BaudRate baud, logging to: $OutFile"
Write-Host "Press Ctrl+C to stop."
Write-Host ""

$writer = New-Object System.IO.StreamWriter($OutFile, $true, [System.Text.Encoding]::UTF8)
$writer.AutoFlush = $true

# Auto-reconnect loop: a device reset, USB re-enumeration, or transient I/O
# glitch throws from ReadLine() (IOException, not TimeoutException) and used
# to kill the whole script. Now any such error just closes the port and
# retries after a short pause, so this keeps running unattended.
try {
    while ($true) {
        $sp = $null
        try {
            $sp = New-Object System.IO.Ports.SerialPort $Port, $BaudRate, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
            $sp.ReadTimeout = 500
            $sp.Open()
            # See .DESCRIPTION above: without these two lines, the Pico's
            # TinyUSB CDC will not send any data at all.
            $sp.DtrEnable = $true
            $sp.RtsEnable = $true

            while ($true) {
                try {
                    $line = $sp.ReadLine()
                } catch [System.TimeoutException] {
                    continue
                }
                if ($null -eq $line) { continue }
                $ts = Get-Date -Format "HH:mm:ss.fff"
                $stamped = "[$ts] $line"
                Write-Host $stamped
                $writer.WriteLine($stamped)
            }
        } catch {
            $ts = Get-Date -Format "HH:mm:ss.fff"
            Write-Host "[$ts] (reconnecting: $($_.Exception.Message))"
            $writer.WriteLine("[$ts] (reconnecting: $($_.Exception.Message))")
        } finally {
            if ($sp -and $sp.IsOpen) { $sp.Close() }
        }
        Start-Sleep -Milliseconds 500
    }
} finally {
    $writer.Close()
    Write-Host ""
    Write-Host "Stopped. Log saved at: $OutFile"
}
