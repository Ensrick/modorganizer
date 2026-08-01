[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$InstanceRoot,
    [Parameter(Mandatory)] [string]$GamePath,
    [switch]$SkipVfs
)

$ErrorActionPreference = 'Stop'
$controller = Join-Path $InstanceRoot 'MO2Headless.exe'
if (-not (Test-Path -LiteralPath $controller -PathType Leaf)) {
    throw "Missing controller: $controller"
}

function Invoke-Headless {
    param([Parameter(ValueFromRemainingArguments)] [string[]]$Arguments)
    $text = & $controller @Arguments
    $code = $LASTEXITCODE
    $result = $text | ConvertFrom-Json
    if ($code -ne 0 -or -not $result.ok) {
        throw "MO2Headless failed ($code): $text"
    }
    return $result
}

$fixtureRoot = Join-Path $InstanceRoot ("headless-smoke-fixtures-" + [guid]::NewGuid().ToString('N'))
$basic = Join-Path $fixtureRoot 'basic'
$fomod = Join-Path $fixtureRoot 'fomod-archive'
New-Item -ItemType Directory -Force -Path $basic, (Join-Path $fomod 'fomod'), (Join-Path $fomod 'Base'), (Join-Path $fomod 'Option') | Out-Null
Set-Content -LiteralPath (Join-Path $basic 'HeadlessSmoke.esp') -Value 'fixture'
Set-Content -LiteralPath (Join-Path $fomod 'fomod\ModuleConfig.xml') -Value '<config />'
Set-Content -LiteralPath (Join-Path $fomod 'Base\HeadlessPlanned.esp') -Value 'fixture'
Set-Content -LiteralPath (Join-Path $fomod 'Option\selected.txt') -Value 'selected'

$archive = Join-Path $fixtureRoot 'planned.zip'
Compress-Archive -Path (Join-Path $fomod '*') -DestinationPath $archive
$plan = Join-Path $fixtureRoot 'plan.json'
@{
    schemaVersion = 1
    mappings = @(
        @{ source = 'Base'; destination = '.' },
        @{ source = 'Option'; destination = '.' }
    )
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $plan

Invoke-Headless init $GamePath | Out-Null
Invoke-Headless profile-create Automation '--clone' Default '--select' | Out-Null
$stage = Invoke-Headless '-p' Automation mod-stage $basic 'Basic Fixture' '--enable'
Invoke-Headless '-p' Automation plugin-enable HeadlessSmoke.esp | Out-Null
$disable = Invoke-Headless '-p' Automation mod-disable 'Basic Fixture'
Invoke-Headless rollback $disable.transaction | Out-Null
Invoke-Headless '-p' Automation mod-install $archive 'Planned Fixture' '--install-plan' $plan '--enable' | Out-Null
Invoke-Headless '-p' Automation audit | Out-Null

if (-not $SkipVfs) {
    $text = & $controller -p Automation --timeout 30 run "$env:SystemRoot\System32\cmd.exe" --arguments '/d /c exit 7'
    $result = $text | ConvertFrom-Json
    if ($LASTEXITCODE -ne 7 -or $result.exitCode -ne 7) {
        throw "VFS exit propagation failed: $text"
    }

    # MO2 adds `*`-marked unmanaged DLC/Creation Club rows during setup. Also
    # prove that a plugin can be disabled after its supplying mod is disabled.
    Invoke-Headless '-p' Automation mod-disable 'Basic Fixture' | Out-Null
    Invoke-Headless '-p' Automation plugin-disable HeadlessSmoke.esp | Out-Null
    Invoke-Headless '-p' Automation audit | Out-Null
}

Write-Host "PASS MO2Headless smoke test (transaction $($stage.transaction))"
