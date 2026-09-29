param([switch]$BuildOnly)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Workspace = (Resolve-Path (Join-Path $Root '..')).Path
$Compiler = Join-Path $Workspace 'build/bin/kelyra.exe'
$ClangDirectory = Join-Path $Workspace 'build/llvm/bin'
$OutputDirectory = Join-Path $Root 'build'
$ShaderSource = Join-Path $PSScriptRoot 'texture_shaders.kly'
$Source = Join-Path $PSScriptRoot 'vulkan_texture_example.kly'
$Output = Join-Path $OutputDirectory 'vulkan_texture_example.exe'

if (-not (Test-Path -LiteralPath $Compiler)) { throw "Missing Kelyra compiler: $Compiler" }
if (-not (Test-Path -LiteralPath (Join-Path $ClangDirectory 'clang.exe'))) { throw "Missing project Clang: $ClangDirectory" }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$env:PATH = "$ClangDirectory;$OutputDirectory;$env:PATH"

& $Compiler --emit-spirv --shader-entry vertex_main "--module-path=$Root/src" `
    -o (Join-Path $OutputDirectory 'vulkan_texture.vert.spv') $ShaderSource
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $Compiler --emit-spirv --shader-entry fragment_main "--module-path=$Root/src" `
    -o (Join-Path $OutputDirectory 'vulkan_texture.frag.spv') $ShaderSource
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$Validator = Join-Path $env:VULKAN_SDK 'Bin/spirv-val.exe'
if (Test-Path -LiteralPath $Validator) {
    & $Validator (Join-Path $OutputDirectory 'vulkan_texture.vert.spv')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $Validator (Join-Path $OutputDirectory 'vulkan_texture.frag.spv')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

& (Join-Path $Root 'native/build-vulkan.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$env:PATH = "$ClangDirectory;$OutputDirectory;$env:PATH"
& $Compiler --emit-exe "--module-path=$Root/src" -o $Output $Source
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $BuildOnly) {
    Push-Location $Root
    try { & $Output; exit $LASTEXITCODE } finally { Pop-Location }
}
