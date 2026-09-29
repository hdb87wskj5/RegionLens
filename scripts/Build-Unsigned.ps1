[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $locator)) { throw 'Visual Studio installer locator was not found.' }
$msbuild = & $locator -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
    Select-Object -First 1
if (!$msbuild) { throw 'MSBuild was not found. Install Visual Studio C++ desktop tools.' }

$commit = 'public-source'
if ((Test-Path -LiteralPath (Join-Path $repository '.git')) -and
    (Get-Command git -ErrorAction SilentlyContinue)) {
    $head = & git -c ("safe.directory=" + $repository.Replace('\','/')) -C $repository rev-parse --short=12 HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $head) { $commit = [string]$head }
}
$properties = @('/m:4','/nologo','/v:minimal','/p:Channel=Stable',
    "/p:Configuration=$Configuration",'/p:Platform=x64',"/p:SourceCommit=$commit")

foreach ($project in @(
    'src\WeTypeProbe\RegionLens.WeTypeProbe.vcxproj',
    'tests\RegionLens.Tests.vcxproj',
    'src\App\RegionLens.vcxproj')) {
    & $msbuild (Join-Path $repository $project) @properties
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $project" }
}

$tests = Join-Path $repository "out\Stable\$Configuration\RegionLens.Tests.exe"
& $tests
if ($LASTEXITCODE -ne 0) { throw "Core tests failed with exit code $LASTEXITCODE" }

Write-Host "Unsigned $Configuration build and fake-input tests passed."
Write-Host 'No installer, certificate, trust-store change or desktop input injection was performed.'
