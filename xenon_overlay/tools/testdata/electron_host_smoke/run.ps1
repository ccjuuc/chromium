param(
  [string]$ChromePath,
  [string]$NodePath = 'node',
  [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
if (-not $ChromePath) { $ChromePath = Join-Path $sourceRoot 'out/Release_64/xlb153.exe' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $sourceRoot 'out/electron-host-smoke' }
$ChromePath = (Resolve-Path -LiteralPath $ChromePath).Path
$NodePath = (Get-Command $NodePath -CommandType Application -ErrorAction Stop).Source
& $NodePath --check (Join-Path $PSScriptRoot 'cdp.cjs')
if ($LASTEXITCODE -ne 0) { throw 'The CDP helper failed its syntax check; no processes were started.' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$profileDirectory = Join-Path $OutputDirectory 'profile'
$existingDumps = @{}
if (Test-Path -LiteralPath $profileDirectory) {
  Get-ChildItem -LiteralPath $profileDirectory -Filter '*.dmp' -Recurse -File |
    ForEach-Object { $existingDumps[$_.FullName] = $true }
}
$listeners = Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
  Where-Object { $_.LocalPort -in @(9223, 9339) }
if ($listeners) { throw 'Smoke ports 9223/9339 are already in use. No processes were started.' }

$server = $null
$browser = $null
try {
  $server = Start-Process -FilePath $NodePath -ArgumentList @('"' + (Join-Path $PSScriptRoot 'server.cjs') + '"') `
    -WorkingDirectory $PSScriptRoot -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $OutputDirectory 'server.stdout.log') `
    -RedirectStandardError (Join-Path $OutputDirectory 'server.stderr.log')
  $ready = $false
  for ($attempt = 0; $attempt -lt 30; $attempt++) {
    try {
      $response = Invoke-WebRequest -Uri 'http://127.0.0.1:9339/health' -TimeoutSec 1
      if ($response.Content -eq 'xenon-game-host-smoke') { $ready = $true; break }
    } catch {}
    Start-Sleep -Milliseconds 100
  }
  if (-not $ready) { throw 'Smoke HTTP server failed to become ready; inspect server.stderr.log.' }
  $browserArguments = @(
    '--xenon-electron-app="' + (Join-Path $PSScriptRoot 'smoke.xenon.json') + '"'
    '--remote-debugging-port=9223'
    '--user-data-dir="' + $profileDirectory + '"'
    '--enable-logging'
    '--log-file="' + (Join-Path $OutputDirectory 'chrome.log') + '"'
    '--no-first-run'
    '--no-default-browser-check'
    'about:blank'
  )
  $browser = Start-Process -FilePath $ChromePath -ArgumentList $browserArguments `
    -WorkingDirectory (Split-Path -Parent $ChromePath) -WindowStyle Hidden -PassThru
  & $NodePath (Join-Path $PSScriptRoot 'cdp.cjs') wait |
    Tee-Object -FilePath (Join-Path $OutputDirectory 'result.json')
  $testExitCode = $LASTEXITCODE
  if ($testExitCode -eq 0) {
    $navigationPending = $false
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
      $pendingResponse = Invoke-WebRequest -Uri 'http://127.0.0.1:9339/pending-navigation' -TimeoutSec 1
      if ($pendingResponse.Content -eq 'true') { $navigationPending = $true; break }
      Start-Sleep -Milliseconds 100
    }
    if (-not $navigationPending) { throw 'The delayed cross-site navigation did not start.' }
  }
} finally {
  if ($browser -and -not $browser.HasExited) {
    & $NodePath (Join-Path $PSScriptRoot 'cdp.cjs') close | Out-Null
    if (-not $browser.WaitForExit(10000)) {
      # Only stop the exact process launched above, never other browsers.
      $browser.Kill()
    }
  }
  if ($server -and -not $server.HasExited) { $server.Kill() }
}
$newDumps = @(Get-ChildItem -LiteralPath $profileDirectory -Filter '*.dmp' -Recurse -File |
  Where-Object { -not $existingDumps.ContainsKey($_.FullName) })
if ($newDumps) {
  $newDumps.FullName | Set-Content -LiteralPath (Join-Path $OutputDirectory 'new-dumps.txt')
  throw 'Browser shutdown generated a new dump; inspect new-dumps.txt.'
}
exit $testExitCode
