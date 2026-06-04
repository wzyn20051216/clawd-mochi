<#
.SYNOPSIS
    安装、检查、测试或卸载 Clawd Mochi 的 Codex / Claude Code 全局桥接器。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -MochiHost 172.20.10.2

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -Action status

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -Action uninstall
#>

[CmdletBinding()]
param(
    [ValidateSet("install", "status", "test", "uninstall")]
    [string]$Action = "install",

    [string]$MochiHost,

    [string]$InstallDir = "$env:USERPROFILE\.codex\mochi-bridge",

    [switch]$NoTest
)

$ErrorActionPreference = "Stop"

$Events = @("SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "Stop")
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$SourceFiles = @("mochi_bridge.py", "mochi_event.py", "agent_mochi.py")
$CodexHome = Join-Path $env:USERPROFILE ".codex"
$ClaudeHome = Join-Path $env:USERPROFILE ".claude"
$CodexHooksPath = Join-Path $CodexHome "hooks.json"
$ClaudeSettingsPath = Join-Path $ClaudeHome "settings.json"

function Write-Info {
    param([string]$Text)
    Write-Host "[Mochi] $Text"
}

function Backup-File {
    param([string]$Path)

    if (Test-Path -LiteralPath $Path) {
        $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
        Copy-Item -LiteralPath $Path -Destination "$Path.bak_mochi_$timestamp" -Force
    }
}

function Read-JsonFile {
    param([string]$Path, [hashtable]$Default)

    if (-not (Test-Path -LiteralPath $Path)) {
        return $Default
    }
    $text = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    if ([string]::IsNullOrWhiteSpace($text)) {
        return $Default
    }
    return $text | ConvertFrom-Json
}

function Save-JsonFile {
    param([string]$Path, [object]$Data)

    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $json = $Data | ConvertTo-Json -Depth 100
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $json + [Environment]::NewLine, $encoding)
}

function Ensure-ObjectProperty {
    param([object]$Object, [string]$Name, [object]$Value)

    if (-not ($Object.PSObject.Properties.Name -contains $Name)) {
        $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    }
}

function New-MochiHooks {
    param([string]$Command)

    $hooks = [ordered]@{}
    foreach ($event in $Events) {
        $hooks[$event] = @(
            [ordered]@{
                matcher = ""
                hooks = @(
                    [ordered]@{
                        type = "command"
                        command = $Command
                    }
                )
            }
        )
    }
    return $hooks
}

function Remove-MochiHooks {
    param([object]$Hooks)

    if ($null -eq $Hooks) {
        return [ordered]@{}
    }

    $result = [ordered]@{}
    foreach ($property in $Hooks.PSObject.Properties) {
        $remainingMatchers = @()
        foreach ($matcherBlock in @($property.Value)) {
            $remainingCommands = @()
            foreach ($hook in @($matcherBlock.hooks)) {
                $command = [string]$hook.command
                if ($command -notmatch "mochi_event\.py") {
                    $remainingCommands += $hook
                }
            }
            if ($remainingCommands.Count -gt 0) {
                $matcherBlock.hooks = $remainingCommands
                $remainingMatchers += $matcherBlock
            }
        }
        if ($remainingMatchers.Count -gt 0) {
            $result[$property.Name] = $remainingMatchers
        }
    }
    return $result
}

function Install-BridgeFiles {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    foreach ($file in $SourceFiles) {
        $source = Join-Path $ProjectRoot "tools\$file"
        if (-not (Test-Path -LiteralPath $source)) {
            throw "Missing source file: $source"
        }
        Copy-Item -LiteralPath $source -Destination (Join-Path $InstallDir $file) -Force
    }
    if ($MochiHost) {
        $encoding = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText((Join-Path $InstallDir ".mochi_host"), $MochiHost.Trim() + [Environment]::NewLine, $encoding)
    } elseif (Test-Path -LiteralPath (Join-Path $ProjectRoot "tools\.mochi_host")) {
        Copy-Item -LiteralPath (Join-Path $ProjectRoot "tools\.mochi_host") -Destination (Join-Path $InstallDir ".mochi_host") -Force
    }
}

function Install-GlobalHooks {
    $eventPath = (Join-Path $InstallDir "mochi_event.py").Replace("\", "/")
    $command = "py -3 `"$eventPath`" --timeout 5"
    $newHooks = New-MochiHooks -Command $command

    New-Item -ItemType Directory -Force -Path $CodexHome | Out-Null
    New-Item -ItemType Directory -Force -Path $ClaudeHome | Out-Null
    Backup-File -Path $CodexHooksPath
    Backup-File -Path $ClaudeSettingsPath

    $codexData = Read-JsonFile -Path $CodexHooksPath -Default @{}
    Ensure-ObjectProperty -Object $codexData -Name "hooks" -Value ([ordered]@{})
    $codexData.hooks = Remove-MochiHooks -Hooks $codexData.hooks
    foreach ($event in $Events) {
        $codexData.hooks[$event] = $newHooks[$event]
    }
    Save-JsonFile -Path $CodexHooksPath -Data $codexData

    $claudeData = Read-JsonFile -Path $ClaudeSettingsPath -Default @{}
    Ensure-ObjectProperty -Object $claudeData -Name "hooks" -Value ([ordered]@{})
    $claudeData.hooks = Remove-MochiHooks -Hooks $claudeData.hooks
    foreach ($event in $Events) {
        $claudeData.hooks[$event] = $newHooks[$event]
    }
    Save-JsonFile -Path $ClaudeSettingsPath -Data $claudeData

    return $command
}

function Uninstall-GlobalHooks {
    Backup-File -Path $CodexHooksPath
    Backup-File -Path $ClaudeSettingsPath

    if (Test-Path -LiteralPath $CodexHooksPath) {
        $codexData = Read-JsonFile -Path $CodexHooksPath -Default @{}
        if ($codexData.PSObject.Properties.Name -contains "hooks") {
            $codexData.hooks = Remove-MochiHooks -Hooks $codexData.hooks
            Save-JsonFile -Path $CodexHooksPath -Data $codexData
        }
    }

    if (Test-Path -LiteralPath $ClaudeSettingsPath) {
        $claudeData = Read-JsonFile -Path $ClaudeSettingsPath -Default @{}
        if ($claudeData.PSObject.Properties.Name -contains "hooks") {
            $claudeData.hooks = Remove-MochiHooks -Hooks $claudeData.hooks
            Save-JsonFile -Path $ClaudeSettingsPath -Data $claudeData
        }
    }
}

function Test-MochiBridge {
    $eventPath = Join-Path $InstallDir "mochi_event.py"
    if (-not (Test-Path -LiteralPath $eventPath)) {
        throw "Bridge is not installed: $eventPath"
    }
    $payload = '{"hook_event_name":"SessionStart","source":"install"}'
    $payload | py -3 $eventPath
    if ($LASTEXITCODE -ne 0) {
        throw "Test event failed"
    }
}

function Show-Status {
    $eventPath = Join-Path $InstallDir "mochi_event.py"
    $hostPath = Join-Path $InstallDir ".mochi_host"
    $hostValue = if (Test-Path -LiteralPath $hostPath) { (Get-Content -LiteralPath $hostPath -Raw).Trim() } else { "(not saved)" }
    $codexOk = (Test-Path -LiteralPath $CodexHooksPath) -and ((Get-Content -LiteralPath $CodexHooksPath -Raw) -match "mochi_event\.py")
    $claudeOk = (Test-Path -LiteralPath $ClaudeSettingsPath) -and ((Get-Content -LiteralPath $ClaudeSettingsPath -Raw) -match "mochi_event\.py")

    Write-Info "Install dir: $InstallDir"
    Write-Info "Bridge files: $([bool](Test-Path -LiteralPath $eventPath))"
    Write-Info "ESP32 host: $hostValue"
    Write-Info "Codex global hook: $codexOk"
    Write-Info "Claude global hook: $claudeOk"
}

switch ($Action) {
    "install" {
        Install-BridgeFiles
        $command = Install-GlobalHooks
        Write-Info "Global bridge installed."
        Write-Info "Hook command: $command"
        Show-Status
        if (-not $NoTest) {
            Test-MochiBridge
            Write-Info "Test event sent."
        }
    }
    "status" {
        Show-Status
    }
    "test" {
        Test-MochiBridge
        Write-Info "Test event sent."
    }
    "uninstall" {
        Uninstall-GlobalHooks
        Write-Info "Codex / Claude global Mochi hooks removed."
        Write-Info "Bridge files kept at: $InstallDir"
    }
}
