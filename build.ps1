# Builds the C++ engine (MSBuild) and the WPF settings window (dotnet) into build\x64\<Configuration>\
param([ValidateSet('Release', 'Debug')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -prerelease -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild (Desktop development with C++) bulunamadi' }

& $msbuild Waveline.sln /p:Configuration=$Configuration /p:Platform=x64 /m /nologo /v:minimal
if ($LASTEXITCODE) { throw 'C++ derlemesi basarisiz' }
& dotnet build src\settings\WavelineSettings.csproj -c $Configuration --nologo -v q
if ($LASTEXITCODE) { throw 'Ayarlar penceresi derlenemedi' }
Write-Host "Tamam: build\x64\$Configuration\WavelineClip.exe"
