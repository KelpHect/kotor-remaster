param(
    [string]$BuildDir = "build/msvc-release",
    [switch]$SkipGpu
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root (Join-Path $BuildDir "kotor_engine.exe")
$logDir = Join-Path $root "logs/smoke"

if (-not (Test-Path $exe)) {
    Write-Error "Executable not found: $exe. Build first (scripts\build.bat)."
    exit 1
}

New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$script:failures = 0
$results = @()

function Invoke-SmokeCase {
    param(
        [string]$Name,
        [string[]]$Arguments,
        [int]$ExpectedExit,
        [string]$ExpectedText,
        [int]$TimeoutSeconds = 30,
        [string]$VideoDriver = ""
    )

    $stdout = Join-Path $logDir "$Name.log"
    $stderr = Join-Path $logDir "$Name.err.log"

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($Arguments -join " ")
    $psi.WorkingDirectory = $root
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    if ($VideoDriver -ne "") {
        $psi.EnvironmentVariables["SDL_VIDEODRIVER"] = $VideoDriver
    }

    $process = [System.Diagnostics.Process]::Start($psi)
    $outTask = $process.StandardOutput.ReadToEndAsync()
    $errTask = $process.StandardError.ReadToEndAsync()
    $finished = $process.WaitForExit($TimeoutSeconds * 1000)

    if (-not $finished) {
        try { $process.Kill() } catch { }
        $process.WaitForExit(5000) | Out-Null
        $script:results += [pscustomobject]@{ Case = $Name; Exit = "TIMEOUT"; Result = "FAIL" }
        $script:failures++
        return
    }

    $text = $outTask.Result + $errTask.Result
    $exitCode = $process.ExitCode
    Set-Content -Path $stdout -Value $outTask.Result -NoNewline -Encoding UTF8
    Set-Content -Path $stderr -Value $errTask.Result -NoNewline -Encoding UTF8

    $textOk = $true
    if ($ExpectedText -ne "") {
        $textOk = $text -match [regex]::Escape($ExpectedText)
    }
    $exitOk = ($exitCode -eq $ExpectedExit)

    if ($exitOk -and $textOk) {
        $script:results += [pscustomobject]@{ Case = $Name; Exit = $exitCode; Result = "PASS" }
    } else {
        $script:results += [pscustomobject]@{ Case = $Name; Exit = $exitCode; Result = "FAIL" }
        $script:failures++
    }
}

Invoke-SmokeCase -Name "imgui-selftest" -Arguments @("--self-test") `
    -ExpectedExit 0 -ExpectedText "ImGui self-test PASSED"

Invoke-SmokeCase -Name "null-headless" -Arguments @("--headless", "--frames", "5", "--seconds", "20") `
    -ExpectedExit 0 -ExpectedText "Backend ready: null" -VideoDriver "dummy"

Invoke-SmokeCase -Name "vulkan-headless-graceful-fail" -Arguments @("--headless", "--vulkan", "--frames", "5") `
    -ExpectedExit 5 -ExpectedText "No usable render backend" -VideoDriver "dummy"

if (-not $SkipGpu) {
    Invoke-SmokeCase -Name "vulkan-120-frames" -Arguments @("--vulkan", "--frames", "120", "--seconds", "10") `
        -ExpectedExit 0 -ExpectedText "Backend summary: vulkan"
    Invoke-SmokeCase -Name "opengl-120-frames" -Arguments @("--gl", "--frames", "120", "--seconds", "10") `
        -ExpectedExit 0 -ExpectedText "Backend summary: opengl"
}

$results | Format-Table -AutoSize | Out-String | Write-Host

if ($script:failures -gt 0) {
    Write-Host ("FAILED: {0} smoke case(s). See {1}" -f $script:failures, $logDir)
    exit 1
}

Write-Host ("PASS: all smoke cases. Logs: {0}" -f $logDir)
exit 0
