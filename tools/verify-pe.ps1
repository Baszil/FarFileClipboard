param(
    [Parameter(Mandatory = $true)][string] $Path,
    [Parameter(Mandatory = $true)][ValidateSet('x86','x64','ARM64')][string] $Expected
)

$resolved = (Resolve-Path -LiteralPath $Path).Path
$bytes = [IO.File]::ReadAllBytes($resolved)
if ($bytes.Length -lt 0x40) { throw "File is too small to be a PE image: $resolved" }
if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { throw "Missing MZ signature: $resolved" }

$peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
if ($peOffset -lt 0 -or ($peOffset + 6) -gt $bytes.Length) { throw "Invalid PE header offset: $resolved" }
if ($bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45 -or $bytes[$peOffset + 2] -ne 0 -or $bytes[$peOffset + 3] -ne 0) {
    throw "Missing PE signature: $resolved"
}

$machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
$actual = switch ($machine) {
    0x014c { 'x86' }
    0x8664 { 'x64' }
    0xAA64 { 'ARM64' }
    default { ('unknown (0x{0:X4})' -f $machine) }
}

Write-Host ("PE machine: {0} ({1})" -f $actual, $resolved)
if ($actual -ne $Expected) {
    throw "PE architecture mismatch: expected $Expected, got $actual"
}
