# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

<#
.SYNOPSIS
  Resets the local application directory to a clean 1.0.0.1 base for update testing.
#>

$rootDir = Resolve-Path "$PSScriptRoot\..\..\.."
$appDir = "C:\Users\Administrator\AppData\Local\xlb153\Application"
$sevenZip = "$rootDir\third_party\lzma_sdk\bin\win64\7za.exe"
$baseInstaller = "$rootDir\test_baselines\1.0.0.1\xlb153_installer_1.0.0.1.exe"
$baseChrome7z = "$rootDir\test_baselines\1.0.0.1\chrome.7z"

Write-Host "[*] Stopping any running xlb153 processes..."
Stop-Process -Name "xlb153" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Host "[*] Cleaning Application directory..."
Remove-Item -Path "$appDir\1.0.0.*" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -Path "$appDir\Chrome-bin" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -Path "$appDir\new_*" -Force -ErrorAction SilentlyContinue
Remove-Item -Path "$appDir\old_*" -Force -ErrorAction SilentlyContinue

Write-Host "[*] Restoring 1.0.0.1 base files..."
& $sevenZip x $baseInstaller "-o$appDir" -y | Out-Null
& $sevenZip x $baseChrome7z "-o$appDir" -y | Out-Null

if (Test-Path "$appDir\Chrome-bin") {
    Get-ChildItem -Path "$appDir\Chrome-bin\*" | ForEach-Object {
        $dest = Join-Path $appDir $_.Name
        if (-not (Test-Path $dest)) {
            Move-Item -Path $_.FullName -Destination $dest -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item -Path "$appDir\Chrome-bin" -Recurse -Force -ErrorAction SilentlyContinue
}

$destSetup = "$appDir\1.0.0.1\Installer\setup.exe"
New-Item -ItemType Directory -Path (Split-Path $destSetup) -Force | Out-Null
if (Test-Path "$appDir\setup.exe") {
    Copy-Item "$appDir\setup.exe" $destSetup -Force
}

Write-Host "[*] Setting registry pv = 1.0.0.1..."
Set-ItemProperty -Path "HKCU:\Software\xlb153" -Name "pv" -Value "1.0.0.1" -Force
Set-ItemProperty -Path "HKCU:\Software\xlb153" -Name "UninstallString" -Value "$destSetup" -Force

Write-Host "[*] Deploying latest updater engine to 1.0.0.1\xlbrowser.dll..."
$compiledDll = "$rootDir\out\Release_64\xlbrowser.dll"
if (Test-Path $compiledDll) {
    Copy-Item $compiledDll "$appDir\1.0.0.1\xlbrowser.dll" -Force
}

Write-Host "[+] Environment successfully reset to 1.0.0.1!"
