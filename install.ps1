param(
    [Parameter(Position = 0)]
    [string] $FarDir,

    [switch] $Worker,
    [switch] $Elevated,
    [string] $Target64
)

$ErrorActionPreference = 'Stop'
$ScriptPath = $PSCommandPath
$InstallerDir = Split-Path -Parent $ScriptPath

function Get-PeArchitecture([string] $Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw ("'{0}' is not a valid PE executable." -f $Path)
    }

    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0 -or ($peOffset + 6) -gt $bytes.Length) {
        throw ("'{0}' has an invalid PE header." -f $Path)
    }

    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    switch ($machine) {
        0x014c { return 'x86' }
        0x8664 { return 'x64' }
        0xAA64 { return 'ARM64' }
        default { throw ("Unsupported PE machine 0x{0:X4} in '{1}'." -f $machine, $Path) }
    }
}

function Get-PluginSource([string] $FarDirectory) {
    # Always decide by the target Far.exe itself.  PowerShell / Windows can be
    # a different architecture, and a universal package may contain all three
    # plugin builds side by side.
    $farExe = Join-Path $FarDirectory 'Far.exe'
    $arch = Get-PeArchitecture $farExe

    # Universal release layout produced by package-all.cmd / GitHub release:
    #   FarFileClipboard-x86\...
    #   FarFileClipboard-x64\...
    #   FarFileClipboard-ARM64\...
    $universal = Join-Path $InstallerDir ("FarFileClipboard-{0}" -f $arch)
    if (Test-Path -LiteralPath (Join-Path $universal 'FarFileClipboard.dll')) {
        return (Resolve-Path -LiteralPath $universal).Path
    }

    # Packaged release layout: install.cmd/install.ps1 are next to a
    # FarFileClipboard directory that already contains the correct DLL.
    $packaged = Join-Path $InstallerDir 'FarFileClipboard'
    if (Test-Path -LiteralPath (Join-Path $packaged 'FarFileClipboard.dll')) {
        return (Resolve-Path -LiteralPath $packaged).Path
    }

    # Development tree layout produced by build.cmd/build-all.cmd.
    # Match the source DLL to the architecture of the Far.exe being updated,
    # not to the architecture of PowerShell or Windows.
    $built = Join-Path $InstallerDir ("dist\FarFileClipboard-{0}" -f $arch)
    if (Test-Path -LiteralPath (Join-Path $built 'FarFileClipboard.dll')) {
        return (Resolve-Path -LiteralPath $built).Path
    }

    throw ("FarFileClipboard.dll for {0} was not found. Run build.cmd {0} (or build-all.cmd), or keep install.cmd/install.ps1 next to the release FarFileClipboard folder." -f $arch)
}

function Get-FarFromAncestors {
    try {
        $processId = $PID
        for ($i = 0; $i -lt 10; $i++) {
            $current = Get-CimInstance Win32_Process -Filter "ProcessId=$processId" -ErrorAction Stop
            if (-not $current -or -not $current.ParentProcessId) { break }

            $parent = Get-CimInstance Win32_Process -Filter "ProcessId=$($current.ParentProcessId)" -ErrorAction Stop
            if (-not $parent) { break }

            if ($parent.Name -ieq 'Far.exe' -and $parent.ExecutablePath) {
                return (Split-Path -Parent $parent.ExecutablePath)
            }

            $processId = [int] $parent.ProcessId
        }
    }
    catch {
        # Ancestor detection is only a convenience; fall back to other methods.
    }

    return $null
}

function Resolve-FarDirectory([string] $Requested) {
    if ($Requested) {
        return [IO.Path]::GetFullPath($Requested.Trim('"'))
    }

    $ancestorFar = Get-FarFromAncestors
    if ($ancestorFar -and (Test-Path -LiteralPath (Join-Path $ancestorFar 'Far.exe'))) {
        return $ancestorFar
    }

    if ($env:FARHOME -and (Test-Path -LiteralPath (Join-Path $env:FARHOME 'Far.exe'))) {
        return [IO.Path]::GetFullPath($env:FARHOME)
    }

    $found = New-Object System.Collections.Generic.List[string]
    $standardRoots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ }
    foreach ($root in $standardRoots) {
        $candidate = Join-Path $root 'Far Manager'
        if (Test-Path -LiteralPath (Join-Path $candidate 'Far.exe')) {
            $full = [IO.Path]::GetFullPath($candidate)
            if (-not $found.Contains($full)) { $found.Add($full) }
        }
    }

    if ($found.Count -eq 1) {
        return $found[0]
    }

    if ($found.Count -gt 1) {
        Write-Host 'More than one Far Manager installation was found:'
        for ($i = 0; $i -lt $found.Count; $i++) {
            Write-Host ("  [{0}] {1}" -f ($i + 1), $found[$i])
        }
        $answer = Read-Host 'Choose installation number'
        $number = 0
        if ([int]::TryParse($answer, [ref] $number) -and $number -ge 1 -and $number -le $found.Count) {
            return $found[$number - 1]
        }
        throw 'No Far Manager installation was selected.'
    }

    $entered = Read-Host 'Far Manager directory'
    if (-not $entered) {
        throw 'Far Manager directory was not specified.'
    }
    return [IO.Path]::GetFullPath($entered.Trim('"'))
}

function Test-WriteAccess([string] $Directory) {
    $plugins = Join-Path $Directory 'Plugins'
    try {
        if (-not (Test-Path -LiteralPath $plugins)) {
            New-Item -ItemType Directory -Path $plugins -Force | Out-Null
        }

        $pluginDir = Join-Path $plugins 'FarFileClipboard'
        $probeDir = if (Test-Path -LiteralPath $pluginDir) { $pluginDir } else { $plugins }
        $probe = Join-Path $probeDir ('.ffc_write_test_' + [guid]::NewGuid().ToString('N') + '.tmp')
        [IO.File]::WriteAllText($probe, 'FarFileClipboard write test', [Text.Encoding]::ASCII)
        Remove-Item -LiteralPath $probe -Force
        return $true
    }
    catch {
        return $false
    }
}

function Test-DllLocked([string] $DllPath) {
    if (-not (Test-Path -LiteralPath $DllPath)) { return $false }

    try {
        $stream = [IO.File]::Open($DllPath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $stream.Close()
        return $false
    }
    catch {
        return $true
    }
}

function Encode-Target([string] $Text) {
    return [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Text))
}

function Decode-Target([string] $Encoded) {
    if (-not $Encoded) { throw 'Worker target is missing.' }
    return [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($Encoded))
}

function Start-Worker([string] $Directory, [bool] $AsAdmin) {
    $encoded = Encode-Target $Directory

    # IMPORTANT: use the script-scoped $PSCommandPath. Inside a function,
    # $MyInvocation.MyCommand.Path refers to the function invocation and is not
    # a reliable path to install.ps1.
    if (-not $ScriptPath -or -not (Test-Path -LiteralPath $ScriptPath)) {
        throw 'Installer script path could not be resolved.'
    }

    # Use -EncodedCommand so paths containing spaces / shell metacharacters do
    # not depend on Start-Process command-line quoting rules.
    $escapedScript = $ScriptPath.Replace("'", "''")
    $escapedTarget = $encoded.Replace("'", "''")
    $command = "& '$escapedScript' -Worker -Target64 '$escapedTarget'"
    if ($AsAdmin) { $command += ' -Elevated' }
    $encodedCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    $argumentLine = "-NoProfile -ExecutionPolicy Bypass -EncodedCommand $encodedCommand"

    $powershellExe = Join-Path $PSHOME 'powershell.exe'
    if (-not (Test-Path -LiteralPath $powershellExe)) {
        $powershellExe = 'powershell.exe'
    }

    $start = @{
        FilePath = $powershellExe
        ArgumentList = $argumentLine
        WorkingDirectory = $InstallerDir
    }
    if ($AsAdmin) { $start.Verb = 'RunAs' }

    try {
        $process = Start-Process @start -PassThru
        if (-not $process) {
            throw 'Start-Process did not return a worker process.'
        }
        return $true
    }
    catch {
        if ($AsAdmin) {
            Write-Host ("ERROR: UAC elevation was cancelled or could not be started: {0}" -f $_.Exception.Message) -ForegroundColor Red
        }
        else {
            Write-Host ("ERROR: could not start detached installer: {0}" -f $_.Exception.Message) -ForegroundColor Red
        }
        return $false
    }
}

function Show-InstallPlan([string] $Source, [string] $Directory) {
    $farExe = Join-Path $Directory 'Far.exe'
    $sourceDll = Join-Path $Source 'FarFileClipboard.dll'
    $targetArch = Get-PeArchitecture $farExe
    $sourceArch = Get-PeArchitecture $sourceDll
    $destination = Join-Path $Directory 'Plugins\FarFileClipboard'

    if ($sourceArch -ne $targetArch) {
        throw ("Architecture mismatch: target Far is {0}, but source FarFileClipboard.dll is {1}.`nTarget: {2}`nSource: {3}" -f $targetArch, $sourceArch, $farExe, $sourceDll)
    }

    Write-Host ''
    Write-Host 'FarFileClipboard install plan:' -ForegroundColor Cyan
    Write-Host ("  Target Far:    {0}" -f $farExe)
    Write-Host ("  Architecture:  {0}" -f $targetArch)
    Write-Host ("  Source build:  {0}" -f $Source)
    Write-Host ("  Source DLL:    {0}" -f $sourceDll)
    Write-Host ("  Destination:   {0}" -f $destination)
    Write-Host ''
}

function Install-Plugin([string] $Source, [string] $Directory) {
    $destination = Join-Path $Directory 'Plugins\FarFileClipboard'
    if (-not (Test-Path -LiteralPath $destination)) {
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
    }

    Copy-Item -Path (Join-Path $Source '*') -Destination $destination -Force -Recurse

    Write-Host ''
    Write-Host 'Installed FarFileClipboard to:' -ForegroundColor Green
    Write-Host ("  {0}" -f $destination)

    $oldFarPaste = Join-Path $Directory 'Plugins\FarPaste'
    $oldCopyPaste = Join-Path $Directory 'Plugins\CopyPaste'
    if (Test-Path -LiteralPath $oldFarPaste) {
        Write-Host 'WARNING: remove the old Plugins\FarPaste folder before starting Far.' -ForegroundColor Yellow
    }
    if (Test-Path -LiteralPath $oldCopyPaste) {
        Write-Host 'WARNING: remove the old Plugins\CopyPaste folder before starting Far.' -ForegroundColor Yellow
    }
    Write-Host 'Restart Far Manager to load the new plugin version.'
    Write-Host 'INSTALL OK.' -ForegroundColor Green
}

try {
    if ($Worker) {
        $FarDir = Decode-Target $Target64
    }
    else {
        $FarDir = Resolve-FarDirectory $FarDir
    }

    if (-not (Test-Path -LiteralPath (Join-Path $FarDir 'Far.exe'))) {
        throw ("Far.exe was not found in '{0}'." -f $FarDir)
    }

    $source = Get-PluginSource $FarDir
    Show-InstallPlan $source $FarDir

    $destination = Join-Path $FarDir 'Plugins\FarFileClipboard'
    $dll = Join-Path $destination 'FarFileClipboard.dll'

    if (-not $Worker) {
        if (-not (Test-WriteAccess $FarDir)) {
            Write-Host 'Administrative rights are required to install into:'
            Write-Host ("  {0}" -f $FarDir)
            Write-Host 'Requesting elevation through Windows UAC...'
            if (-not (Start-Worker $FarDir $true)) { exit 1 }
            exit 0
        }

        if (Test-DllLocked $dll) {
            Write-Host 'FarFileClipboard.dll is currently loaded by Far Manager.'
            Write-Host 'Continuing installation in a separate window...'
            if (-not (Start-Worker $FarDir $false)) { exit 1 }
            exit 0
        }
    }
    else {
        if (-not (Test-WriteAccess $FarDir)) {
            if (-not $Elevated) {
                Write-Host 'Administrative rights are required. Requesting Windows UAC...'
                if (-not (Start-Worker $FarDir $true)) { exit 1 }
                exit 0
            }
            throw ("No write access to '{0}\Plugins' even after elevation." -f $FarDir)
        }

        if (Test-DllLocked $dll) {
            Write-Host ''
            Write-Host 'FarFileClipboard.dll is loaded by Far Manager.' -ForegroundColor Yellow
            Write-Host 'Close every Far Manager window that uses this installation:'
            Write-Host ("  {0}" -f $FarDir)
            Write-Host ''
            Write-Host 'Waiting for the DLL to be released. Press Ctrl+C to cancel.'
            while (Test-DllLocked $dll) {
                Start-Sleep -Milliseconds 700
            }
            Write-Host 'DLL released. Continuing installation...'
        }
    }

    Install-Plugin $source $FarDir

    if ($Worker) {
        Write-Host ''
        [void] (Read-Host 'Press Enter to close')
    }
    exit 0
}
catch {
    Write-Host ''
    Write-Host ("INSTALL FAILED: {0}" -f $_.Exception.Message) -ForegroundColor Red
    if ($Worker) {
        Write-Host ''
        [void] (Read-Host 'Press Enter to close')
    }
    exit 1
}
