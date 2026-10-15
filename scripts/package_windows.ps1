# package_windows.ps1 - Package Windows binaries and configuration
$pkgName = "modbus-gateway-win64"
$distDir = "dist/$pkgName"

Write-Host "Creating Windows deployment package in $distDir..." -ForegroundColor Cyan
if (Test-Path "dist") { Remove-Item "dist" -Recurse -Force }
New-Item -ItemType Directory -Path "$distDir/bin", "$distDir/config", "$distDir/docs" -Force | Out-Null

Copy-Item "gateway_cli.exe" "$distDir/bin/"
Copy-Item "slave_simulator.exe" "$distDir/bin/"
Copy-Item "config/*" "$distDir/config/" -Recurse
Copy-Item "README.md" "$distDir/"
Copy-Item "docs/*" "$distDir/docs/" -Recurse

Compress-Archive -Path "$distDir" -DestinationPath "dist/$pkgName.zip" -Force
Write-Host "[SUCCESS] Generated package: dist/$pkgName.zip" -ForegroundColor Green
