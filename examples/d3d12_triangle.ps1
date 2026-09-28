param(
    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Workspace = (Resolve-Path (Join-Path $Root '..')).Path
$Compiler = Join-Path $Workspace 'kelyra/build/bin/kelyra.exe'
$ClangDirectory = Join-Path $Workspace 'kelyra/build/llvm/bin'
$Source = Join-Path $PSScriptRoot 'd3d12_triangle_example.kly'
$ShaderSource = Join-Path $PSScriptRoot 'triangle_shaders.kly'
$OutputDirectory = Join-Path $Root 'build'
$Output = Join-Path $OutputDirectory 'd3d12_triangle_example.exe'

if (-not (Test-Path -LiteralPath $Compiler)) {
    throw "Kelyra compiler is missing: $Compiler"
}
if (-not (Test-Path -LiteralPath (Join-Path $ClangDirectory 'clang.exe'))) {
    throw "Project Clang is missing: $ClangDirectory"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$env:PATH = "$ClangDirectory;$env:PATH"

& $Compiler --emit-dxil --shader-entry vertex_main "--module-path=$Root/src" "--module-path=$PSScriptRoot" -o (Join-Path $OutputDirectory 'triangle.vs.dxil') $ShaderSource
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& $Compiler --emit-dxil --shader-entry fragment_main "--module-path=$Root/src" "--module-path=$PSScriptRoot" -o (Join-Path $OutputDirectory 'triangle.ps.dxil') $ShaderSource
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Compiler --emit-exe "--module-path=$Root/src" -o $Output $Source
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
if (-not $BuildOnly) {
    Push-Location $Root
    try { & $Output; exit $LASTEXITCODE } finally { Pop-Location }
}
