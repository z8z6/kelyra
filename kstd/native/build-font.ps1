$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$Output = Join-Path $Repo 'kstd/build'
$Fonts = Join-Path $Repo 'build/fonts'
$Prefix = Join-Path $Fonts 'prefix'
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$VisualStudio = (& $VsWhere -latest -products '*' -property installationPath | Select-Object -First 1)
if (-not $VisualStudio) { throw 'Visual Studio C++ tools are missing.' }
$VcVars = Join-Path $VisualStudio 'VC/Auxiliary/Build/vcvars64.bat'
$EnvironmentLines = & cmd.exe /d /s /c "call `"$VcVars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the Visual Studio C++ environment.' }
foreach ($Line in $EnvironmentLines) {
    if ($Line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
New-Item -ItemType Directory -Path $Output, $Fonts, $Prefix -Force | Out-Null
$Cmake = (Get-Command cmake.exe).Source

& $Cmake -S (Join-Path $Repo 'third_party/freetype') -B (Join-Path $Fonts 'freetype') -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_FLAGS_RELEASE=/MT /O2 /Ob2 /DNDEBUG" "-DCMAKE_INSTALL_PREFIX=$Prefix" -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DBUILD_SHARED_LIBS=OFF
if ($LASTEXITCODE -ne 0) { throw 'FreeType configuration failed.' }
& $Cmake --build (Join-Path $Fonts 'freetype') --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'FreeType build failed.' }
& $Cmake --install (Join-Path $Fonts 'freetype')
if ($LASTEXITCODE -ne 0) { throw 'FreeType install failed.' }

& $Cmake -S (Join-Path $Repo 'third_party/harfbuzz') -B (Join-Path $Fonts 'harfbuzz') -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS_RELEASE=/MT /O2 /Ob2 /DNDEBUG" "-DCMAKE_INSTALL_PREFIX=$Prefix" "-DCMAKE_PREFIX_PATH=$Prefix" -DHB_HAVE_FREETYPE=ON -DHB_HAVE_ICU=OFF -DHB_BUILD_SUBSET=OFF -DHB_BUILD_UTILS=OFF -DBUILD_SHARED_LIBS=OFF
if ($LASTEXITCODE -ne 0) { throw 'HarfBuzz configuration failed.' }
& $Cmake --build (Join-Path $Fonts 'harfbuzz') --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'HarfBuzz build failed.' }
& $Cmake --install (Join-Path $Fonts 'harfbuzz')
if ($LASTEXITCODE -ne 0) { throw 'HarfBuzz install failed.' }

# ICU's MSBuild projects write relative paths into their source tree. Build a
# local copy so the fixed-version submodule remains clean.
$IcuSource = Join-Path $Fonts 'icu-source'
if (-not (Test-Path -LiteralPath (Join-Path $IcuSource 'source/allinone/allinone.sln'))) {
    New-Item -ItemType Directory -Path $IcuSource -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $Repo 'third_party/icu/icu4c/source') -Destination $IcuSource -Recurse -Force
}
$MsBuild = Join-Path $VisualStudio 'MSBuild/Current/Bin/MSBuild.exe'
$IcuSolution = Join-Path $IcuSource 'source/allinone/allinone.sln'
$IcuDlls = @('icudt78.dll', 'icuuc78.dll', 'icuin78.dll')
if (@($IcuDlls | Where-Object { -not (Test-Path -LiteralPath (Join-Path $IcuSource "bin64/$_")) }).Count -gt 0) {
    & $MsBuild $IcuSolution /m:2 /p:Configuration=Release /p:Platform=x64 /p:SkipUWP=true /p:DefaultPlatformToolset=v145 /v:minimal
    if ($LASTEXITCODE -ne 0) { throw 'ICU build failed.' }
}

Copy-Item -LiteralPath (Join-Path $IcuSource 'bin64/icuuc78.dll') -Destination $Output -Force
Copy-Item -LiteralPath (Join-Path $IcuSource 'bin64/icuin78.dll') -Destination $Output -Force
Copy-Item -LiteralPath (Join-Path $IcuSource 'bin64/icudt78.dll') -Destination $Output -Force
