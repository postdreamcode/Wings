# Opens the probe board's CDC port, triggers a re-run, and streams output.
# DTR/RTS are left at their idle defaults: on ESP32-S3 USB-Serial-JTAG those
# lines control reset and download mode, so asserting them here could reboot
# the board or drop it into the bootloader.
param(
  [string]$Port = "COM8",
  [int]$Seconds = 45
)

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$sp.ReadTimeout = 500
$sp.NewLine = "`n"

try {
  $sp.Open()
} catch {
  Write-Error "Could not open $Port : $_"
  exit 1
}

Start-Sleep -Milliseconds 2000
try { $sp.Write("`n") } catch { }

$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
  try {
    $chunk = $sp.ReadExisting()
    if ($chunk) { Write-Host -NoNewline $chunk }
  } catch { }
  Start-Sleep -Milliseconds 40
}

$sp.Close()
