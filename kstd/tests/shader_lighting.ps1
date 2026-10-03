$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Workspace = (Resolve-Path (Join-Path $Root '..')).Path
$Compiler = Join-Path $Workspace 'build/bin/kelyra.exe'
$Source = Join-Path $Root 'examples/shader_lighting_example.kly'
$OutputDirectory = Join-Path $Root 'build/shader_lighting'

if (-not (Test-Path -LiteralPath $Compiler)) {
    throw "Kelyra compiler is missing: $Compiler"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

foreach ($Target in @('dxil', 'spirv')) {
    $Directory = Join-Path $OutputDirectory $Target
    & $Compiler "--shader-format=$Target" "--module-search-path=$Root/src" -o $Directory $Source
    if ($LASTEXITCODE -ne 0) { throw "Shader compilation failed: $Target" }
    $Extension = if ($Target -eq 'spirv') { 'spv' } else { 'dxil' }
    foreach ($Stage in @('vertex', 'fragment')) {
        $Suffix = if ($Stage -eq 'vertex') { 'vert' } else { 'frag' }
        $Output = Join-Path $Directory "shader_lighting_example.${Stage}_main.$Suffix.$Extension"
        $Bytes = [System.IO.File]::ReadAllBytes($Output)
        if ($Bytes.Length -lt 4) { throw "Shader output is empty: $Output" }
        if ($Target -eq 'spirv') {
            if ([BitConverter]::ToUInt32($Bytes, 0) -ne 0x07230203) {
                throw "Output is not SPIR-V: $Output"
            }
        } elseif ([System.Text.Encoding]::ASCII.GetString($Bytes, 0, 4) -ne 'DXBC') {
            throw "Output is not a DXIL container: $Output"
        }
    }
}
