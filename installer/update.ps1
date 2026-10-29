<#
 SciFi Shell updater. Checks the latest GitHub release of the public repo, and if it is newer than version.txt,
 asks before downloading and running the installer (which closes and restarts the shell).

 No GitHub account or token is needed.
 -Interactive also reports "up to date" / errors; without it the check is silent unless an update exists.
#>
param([switch]$Interactive)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
$repo = 'Davidalanfisk/SciFiShell'

function Say($msg, $icon = 'Information') {
    if ($Interactive) { [void][Windows.Forms.MessageBox]::Show($msg, 'SciFi Shell', 'OK', $icon) }
}

try {
    $current = [version]((Get-Content (Join-Path $PSScriptRoot 'version.txt') -Raw).Trim())

    $h = @{ 'User-Agent' = 'SciFiShell-Updater'; 'X-GitHub-Api-Version' = '2022-11-28' }
    $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" -Headers ($h + @{ Accept = 'application/vnd.github+json' })
    $latest = [version]($rel.tag_name.TrimStart('v'))
    if ($latest -le $current) { Say "SciFi Shell $current is up to date."; exit 0 }

    $asset = $rel.assets | Where-Object { $_.name -like 'SciFiShell-Setup-*.exe' } | Select-Object -First 1
    if (-not $asset) { Say "Release $latest has no installer attached." 'Warning'; exit 1 }
    $ask = [Windows.Forms.MessageBox]::Show("SciFi Shell $latest is available (you have $current).`nDownload and install now? The shell will restart.", 'SciFi Shell update', 'YesNo', 'Question')
    if ($ask -ne 'Yes') { exit 0 }

    $dest = Join-Path $env:TEMP $asset.name
    Invoke-WebRequest $asset.browser_download_url -Headers $h -OutFile $dest
    Start-Process $dest -ArgumentList '/SILENT', '/CLOSEAPPLICATIONS', '/RESTARTAPPLICATIONS'
} catch {
    Say "Update check failed:`n$($_.Exception.Message)" 'Error'
    exit 1
}
