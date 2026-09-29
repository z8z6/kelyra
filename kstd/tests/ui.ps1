$ErrorActionPreference = 'Stop'

$StandardLibrary = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Workspace = (Resolve-Path (Join-Path $StandardLibrary '..')).Path
$Compiler = Join-Path $Workspace 'build/bin/kelyra.exe'
$OutputDirectory = Join-Path $Workspace 'build/kstd-ui'

if (-not (Test-Path -LiteralPath $Compiler)) {
    throw "Kelyra compiler is missing: $Compiler"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

foreach ($Name in @(
    'ui_component_example',
    'ui_layout_example',
    'ui_win_smoke_example',
    'ui_input_smoke_example'
)) {
    $Source = Join-Path $StandardLibrary "examples/$Name.kly"
    $Output = Join-Path $OutputDirectory "$Name.exe"
    & $Compiler --emit-exe "--module-path=$StandardLibrary/src" -o $Output $Source
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $Name" }
    & $Output
    if ($LASTEXITCODE -ne 0) { throw "Example failed: $Name ($LASTEXITCODE)" }
}
