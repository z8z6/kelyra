$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Build = Join-Path $Root 'build'
$Capture = Join-Path $Build 'vulkan_texture_test.ppm'
$Output = Join-Path $Build 'vulkan_texture_test.out'
$ErrorOutput = Join-Path $Build 'vulkan_texture_test.err'

& (Join-Path $Root 'examples/vulkan_texture.ps1') -BuildOnly
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (Test-Path -LiteralPath $Capture) { Remove-Item -LiteralPath $Capture }

$PreviousCapture = $env:KSTD_VULKAN_CAPTURE
$Process = $null
try {
    $env:KSTD_VULKAN_CAPTURE = $Capture
    $Process = Start-Process -FilePath (Join-Path $Build 'vulkan_texture_example.exe') `
        -WorkingDirectory $Root -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $Output -RedirectStandardError $ErrorOutput
    $Deadline = [DateTime]::UtcNow.AddSeconds(20)
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
            throw "Vulkan texture example exited with $($Process.ExitCode): $(Get-Content $Output -Raw)"
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path -LiteralPath $Capture)) { throw 'No Vulkan texture frame was captured.' }
    $Bytes = [System.IO.File]::ReadAllBytes($Capture)
    $Header = [System.Text.Encoding]::ASCII.GetString($Bytes, 0, [Math]::Min(64, $Bytes.Length))
    if ($Header -notmatch '^P6\n(\d+) (\d+)\n255\n') { throw 'Invalid PPM frame header.' }
    $Width = [int]$Matches[1]
    $Height = [int]$Matches[2]
    $Offset = $Matches[0].Length
    if ($Bytes.Length -ne $Offset + $Width * $Height * 3) { throw 'Incomplete Vulkan frame.' }
    function Assert-Pixel([int]$X, [int]$Y, [int]$R, [int]$G, [int]$B) {
        $Index = $Offset + ($Y * $Width + $X) * 3
        $Actual = @([int]$Bytes[$Index], [int]$Bytes[$Index + 1], [int]$Bytes[$Index + 2])
        $Expected = @($R, $G, $B)
        for ($Channel = 0; $Channel -lt 3; $Channel++) {
            if ([Math]::Abs($Actual[$Channel] - $Expected[$Channel]) -gt 55) {
                throw "Pixel ($X,$Y) expected $Expected but got $Actual"
            }
        }
    }
    Assert-Pixel 200 150 255 0 0
    Assert-Pixel 600 150 0 255 0
    Assert-Pixel 200 450 0 0 255
    Assert-Pixel 600 450 255 255 0
    Write-Output "Vulkan RGBA8 texture rendered: ${Width}x${Height}"
} finally {
    if ($Process -and -not $Process.HasExited) { Stop-Process -Id $Process.Id }
    $env:KSTD_VULKAN_CAPTURE = $PreviousCapture
}
