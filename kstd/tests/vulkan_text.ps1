$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Build = Join-Path $Root 'build'
$Capture = Join-Path $Build 'vulkan_text_test.ppm'
$Stdout = Join-Path $Build 'vulkan_text_test.out'
$Stderr = Join-Path $Build 'vulkan_text_test.err'

& (Join-Path $Root 'examples/vulkan_text.ps1') -BuildOnly
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$Workspace = (Resolve-Path (Join-Path $Root '..')).Path
$FontLib = Join-Path $Workspace 'build/fonts/prefix/lib'
$IcuLib = Join-Path $Workspace 'build/fonts/icu-source/lib64'
$LayoutTest = Join-Path $Build 'font_layout_test.exe'
& (Join-Path $Workspace 'build/bin/kelyra.exe') --emit-exe `
    "--module-search-path=$(Join-Path $Root 'src')" `
    "--link-input=$(Join-Path $FontLib 'harfbuzz.lib')" `
    "--link-input=$(Join-Path $FontLib 'freetype.lib')" `
    "--link-input=$(Join-Path $IcuLib 'icuuc.lib')" `
    "--link-input=$(Join-Path $IcuLib 'icuin.lib')" `
    -o $LayoutTest (Join-Path $PSScriptRoot 'font_layout_test.kly')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $LayoutTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (Test-Path -LiteralPath $Capture) { Remove-Item -LiteralPath $Capture }

$PreviousPath = $env:PATH
$PreviousCapture = $env:KSTD_VULKAN_CAPTURE
$Process = $null
try {
    $env:PATH = "$Build;$(Join-Path $Root '../build/llvm/bin');$env:PATH"
    $env:KSTD_VULKAN_CAPTURE = $Capture
    $Process = Start-Process -FilePath (Join-Path $Build 'vulkan_text_example.exe') `
        -WorkingDirectory $Root -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $Stdout -RedirectStandardError $Stderr
    $Deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $Deadline) {
        if (Test-Path -LiteralPath $Capture) {
            try {
                if ((Get-Item -LiteralPath $Capture).Length -eq 1440015) { break }
            } catch [System.IO.IOException] { }
        }
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "Vulkan text example exited with $($Process.ExitCode): $(Get-Content $Stdout -Raw)"
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path -LiteralPath $Capture)) { throw 'No Vulkan text frame was captured.' }
    & python (Join-Path $PSScriptRoot 'vulkan_text_check.py') $Capture
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    if ($Process -and -not $Process.HasExited) { Stop-Process -Id $Process.Id }
    $env:KSTD_VULKAN_CAPTURE = $PreviousCapture
    $env:PATH = $PreviousPath
}
