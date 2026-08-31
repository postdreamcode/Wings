# Sends serial console commands to the wing board and streams the reply.
# DTR/RTS are left at their .NET defaults (both false): on ESP32-S3
# USB-Serial-JTAG those lines drive reset and download mode, so asserting them
# would reboot the board and wipe the RAM trace ring we are trying to read.
param(
  [string]$Port = "COM8",
  [string[]]$Cmd = @("status"),
  [int]$Seconds = 8
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

Start-Sleep -Milliseconds 1200
$sp.ReadExisting() | Out-Null

# -File passes "a,b" as one token, so split here rather than relying on binding.
$lines = @()
foreach ($c in $Cmd) { $lines += ($c -split ',') }

foreach ($c in $lines) {
  $t = $c.Trim()
  if ($t.Length -eq 0) { continue }
  $sp.Write("$t`n")
  Start-Sleep -Milliseconds 600
}

$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
  try {
    $chunk = $sp.ReadExisting()
    if ($chunk) { Write-Host -NoNewline $chunk }
  } catch { }
  Start-Sleep -Milliseconds 40
}

$sp.Close()
