$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Build = Join-Path $Root 'build'
$Capture = Join-Path $Build 'vulkan_triangle_test.ppm'
$Output = Join-Path $Build 'vulkan_triangle_test.out'
$ErrorOutput = Join-Path $Build 'vulkan_triangle_test.err'

& (Join-Path $Root 'examples/vulkan_triangle.ps1') -BuildOnly
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (Test-Path -LiteralPath $Capture) { Remove-Item -LiteralPath $Capture }

$PreviousCapture = $env:KSTD_VULKAN_CAPTURE
$Process = $null
try {
    $env:KSTD_VULKAN_CAPTURE = $Capture
    $Process = Start-Process -FilePath (Join-Path $Build 'vulkan_triangle_example.exe') `
        -WorkingDirectory $Root -WindowStyle Normal -PassThru `
        -RedirectStandardOutput $Output -RedirectStandardError $ErrorOutput
    $Deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $Deadline) {
        if (Test-Path -LiteralPath $Capture) {
            try {
                $Bytes = [System.IO.File]::ReadAllBytes($Capture)
                if ($Bytes.Length -gt 1000000) { break }
            } catch [System.IO.IOException] {
                # The adapter may still be writing the first frame.
            }
        }
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "Vulkan triangle exited with $($Process.ExitCode): $(Get-Content $Output -Raw)"
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path -LiteralPath $Capture)) { throw 'No Vulkan frame was captured.' }
    $Bytes = [System.IO.File]::ReadAllBytes($Capture)
    $Header = [System.Text.Encoding]::ASCII.GetString($Bytes, 0, [Math]::Min(64, $Bytes.Length))
    if ($Header -notmatch '^P6\n(\d+) (\d+)\n255\n') { throw 'Invalid PPM frame header.' }
    $Width = [int]$Matches[1]
    $Height = [int]$Matches[2]
    $Offset = $Matches[0].Length
    if ($Bytes.Length -ne $Offset + $Width * $Height * 3) { throw 'Incomplete Vulkan frame.' }
    $Center = $Offset + (300 * $Width + 400) * 3
    $Corner = $Offset + (4 * $Width + 4) * 3
    $BelowTriangle = $Offset + (520 * $Width + 400) * 3
    if ($Bytes[$Center] -lt 200 -or $Bytes[$Center + 1] -gt 100 -or
        $Bytes[$Corner] -gt 50 -or $Bytes[$BelowTriangle] -gt 50) {
        throw 'Captured frame has no upright triangle.'
    }
    Write-Output "Vulkan triangle rendered: ${Width}x${Height}"
} finally {
    if ($Process -and -not $Process.HasExited) { Stop-Process -Id $Process.Id }
    $env:KSTD_VULKAN_CAPTURE = $PreviousCapture
}
