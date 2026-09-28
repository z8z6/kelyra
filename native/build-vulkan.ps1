$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Sdk = $env:VULKAN_SDK
if (-not $Sdk) {
    throw 'Set VULKAN_SDK to the installed Vulkan SDK directory.'
}
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $VsWhere)) {
    throw "Visual Studio discovery tool is missing: $VsWhere"
}
$VisualStudio = (& $VsWhere -latest -products '*' -property installationPath | Select-Object -First 1)
if (-not $VisualStudio) {
    throw 'Visual Studio C++ tools are missing.'
}
$VcVars = Join-Path $VisualStudio 'VC/Auxiliary/Build/vcvars64.bat'
$EnvironmentLines = & cmd.exe /d /s /c "call `"$VcVars`" >nul && set"
if ($LASTEXITCODE -ne 0) {
    throw 'Could not initialize the Visual Studio C++ environment.'
}
foreach ($Line in $EnvironmentLines) {
    if ($Line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$OutputDirectory = Join-Path $Root 'build'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Push-Location $Root
try {
    $Compiler = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/cl.exe'
    & $Compiler /nologo /std:c++20 /EHsc /MD /O2 "/I$Sdk/Include" "/Fo$OutputDirectory/vulkan_window.obj" /LD native/vulkan_window.cpp /link "/LIBPATH:$Sdk/Lib" vulkan-1.lib user32.lib "/OUT:$OutputDirectory/kstd_vulkan_window.dll" "/IMPLIB:$OutputDirectory/kstd_vulkan_window.lib"
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
