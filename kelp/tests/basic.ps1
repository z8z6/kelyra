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
            $compiler = Join-Path (Split-Path -Parent $Kelp) 'kelyra.exe'
            if (Test-Path -LiteralPath $compiler) {
                $compiler = $compiler.Replace('\', '/')
                $manifest = [System.IO.File]::ReadAllText((Join-Path (Get-Location) 'kelp.toml'))
                $manifest = $manifest.Replace('compiler = "kelyra"', "compiler = `"$compiler`"")
                [System.IO.File]::WriteAllText((Join-Path (Get-Location) 'kelp.toml'), $manifest)
                $sourcePath = Join-Path (Get-Location) 'src/main.kly'
                $originalSource = [System.IO.File]::ReadAllText($sourcePath)
                $crossManifest = $manifest.Replace('[build]', "[build]`ntarget = `"x86_64-unknown-linux-gnu`"")
                $crossManifest = $crossManifest.Replace('c-args = []', 'c-args = ["-DKELP_CHECK=1"]')
                [System.IO.File]::WriteAllText((Join-Path (Get-Location) 'kelp.toml'), $crossManifest)
                [System.IO.File]::WriteAllText((Join-Path (Get-Location) 'src/check.h'), '
#ifndef KELP_CHECK
#error missing C frontend options
#endif
#ifdef _WIN32
#error wrong C frontend target
#endif
int kelp_check(void);
')
                [System.IO.File]::WriteAllText($sourcePath, '
import c "check.h";
@cfg(os.Linux)
fn linux_value() -> i32 { return c.kelp_check(); }
@main
pub fn main() -> i32 { return linux_value(); }
')
                foreach ($action in @('check', 'test')) {
                    & $Kelp $action
                    if ($LASTEXITCODE -ne 0) { throw "kelp $action did not pass target and C frontend options" }
                }
                [System.IO.File]::WriteAllText((Join-Path (Get-Location) 'kelp.toml'), $manifest)
                [System.IO.File]::WriteAllText($sourcePath, $originalSource)
                foreach ($action in @('check', 'build', 'test', 'run')) {
                    & $Kelp $action
                    if ($LASTEXITCODE -ne 0) { throw "kelp $action failed with the current compiler" }
                }
                if (-not (Test-Path -LiteralPath $output)) {
                    throw 'kelp build did not produce the configured artifact'
                }
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
