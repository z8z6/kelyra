param([Parameter(Mandatory = $true)][string]$Kelp)

$ErrorActionPreference = 'Stop'
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("kelp-test-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $version = & $Kelp --version
    if ($LASTEXITCODE -ne 0 -or $version -ne 'kelp 0.1.0') {
        throw "Unexpected Kelp version: $version"
    }

    Push-Location $testRoot
    try {
        & $Kelp new demo
        if ($LASTEXITCODE -ne 0) { throw 'kelp new failed' }
        if (-not (Test-Path 'demo/kelp.toml') -or -not (Test-Path 'demo/src/main.kly')) {
            throw 'kelp new did not create the project files'
        }
        if (-not (Select-String -Path 'demo/kelp.toml' -Pattern '^output = "demo"$' -Quiet)) {
            throw 'Generated manifest has the wrong output name'
        }
        Push-Location 'demo'
        try {
            $output = & $Kelp output
            if ($LASTEXITCODE -ne 0 -or $output -notmatch 'demo(\.exe)?$') {
                throw "kelp output returned an unexpected path: $output"
            }
        } finally {
            Pop-Location
        }
    } finally {
        Pop-Location
    }
} finally {
    $resolvedTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\', '/')
    $resolvedTest = [System.IO.Path]::GetFullPath($testRoot)
    if (-not $resolvedTest.StartsWith($resolvedTemp + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a directory outside the temporary directory: $resolvedTest"
    }
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}
