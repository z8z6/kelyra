$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Workspace = (Resolve-Path (Join-Path $Root '..')).Path
$Compiler = Join-Path $Workspace 'kelyra/build/bin/kelyra.exe'
$ClangDirectory = Join-Path $Workspace 'kelyra/build/llvm/bin'
$ShaderSource = Join-Path $Root 'examples/triangle_shaders.kly'
$TestSource = Join-Path $PSScriptRoot 'd3d12_triangle_readback.kly'
$OutputDirectory = Join-Path $Root 'build'
$TestExe = Join-Path $OutputDirectory 'd3d12_triangle_readback.exe'

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$env:PATH = "$ClangDirectory;$env:PATH"
& $Compiler --emit-dxil --shader-entry vertex_main "--module-path=$Root/src" "--module-path=$Root/examples" -o (Join-Path $OutputDirectory 'triangle.vs.dxil') $ShaderSource
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $Compiler --emit-dxil --shader-entry fragment_main "--module-path=$Root/src" "--module-path=$Root/examples" -o (Join-Path $OutputDirectory 'triangle.ps.dxil') $ShaderSource
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $Compiler --emit-exe "--module-path=$Root/src" -o $TestExe $TestSource
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location $Root
try {
    $Output = & $TestExe
    $Status = $LASTEXITCODE
} finally {
    Pop-Location
}
$Output | Write-Output
if ($Status -ne 0) { exit $Status }
if ($Output -notmatch 'DX12 GPU triangle center RGB: (\d+) (\d+) (\d+)') {
    throw "Unexpected DX12 GPU readback: $Output"
}
$Red = [int]$Matches[1]
$Green = [int]$Matches[2]
$Blue = [int]$Matches[3]
if ($Red -lt 220 -or $Red -gt 235 -or
    $Green -lt 45 -or $Green -gt 60 -or
    $Blue -lt 68 -or $Blue -gt 85) {
    throw "DX12 triangle color does not match the shader: $Output"
}
