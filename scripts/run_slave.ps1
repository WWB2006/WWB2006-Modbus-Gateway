# run_slave.ps1 - Start the slave simulator on Windows
param(
    [int]$Port = 5020,
    [int]$Unit = 1,
    [string]$Profile = ""
)

$exe = if (Test-Path ".\slave_simulator.exe") { ".\slave_simulator.exe" } else { ".\build\src\slave_simulator.exe" }

$argsList = @("-p", $Port, "-u", $Unit)
if ($Profile -ne "") {
    $argsList += @("-f", $Profile)
}

Write-Host "Launching Slave Simulator on port $Port (Unit $Unit)..." -ForegroundColor Cyan
& $exe @argsList
