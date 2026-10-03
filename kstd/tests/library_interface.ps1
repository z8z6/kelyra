$ErrorActionPreference = 'Stop'

$StandardLibrary = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Workspace = (Resolve-Path (Join-Path $StandardLibrary '..')).Path
$Compiler = Join-Path $Workspace 'build/bin/kelyra.exe'
$Kelp = Join-Path $Workspace 'build/bin/kelp.exe'
$OutputDirectory = Join-Path $Workspace 'build/kstd-interface'
$Output = Join-Path $OutputDirectory 'ascii.exe'

if (-not (Test-Path -LiteralPath $Compiler) -or
    -not (Test-Path -LiteralPath $Kelp)) {
  throw 'Build kelyra and kelp before running this test.'
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
Push-Location $StandardLibrary
try {
  & $Kelp build
  if ($LASTEXITCODE -ne 0) { throw 'kstd library build failed' }
  $Library = & $Kelp output
  if ($LASTEXITCODE -ne 0) { throw 'kstd library output query failed' }
  $Interface = "$Library.kmi"
} finally {
  Pop-Location
}

& $Compiler --emit-exe "--module-interface-path=$Interface" `
  "--link=$Library" -o $Output `
  (Join-Path $StandardLibrary 'examples/ascii_example.kly')
if ($LASTEXITCODE -ne 0) { throw 'kstd interface consumer build failed' }
& $Output
if ($LASTEXITCODE -ne 0) { throw "kstd interface consumer returned $LASTEXITCODE" }
