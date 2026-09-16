# Non-formatting physical regression. Reformat is a separate, explicitly
# authorized step. Identity is checked before creating any native fixtures.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Z]:$')][string]$Drive,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{8}$')][string]$ExpectedSerial,
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$ExpectedDeviceSerial,
    [Parameter(Mandatory)][string]$EvidenceDirectory
)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$evidence=[IO.Path]::GetFullPath($EvidenceDirectory)
if ($evidence.StartsWith($Drive,[StringComparison]::OrdinalIgnoreCase)) { throw 'Keep evidence off the device' }
if (Test-Path -LiteralPath $evidence) { throw 'Choose a new evidence directory' }
function Assert-Identity {
    $logical=Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$Drive'"
    $parts=@(Get-CimAssociatedInstance -InputObject $logical -Association Win32_LogicalDiskToPartition)
    if ($parts.Count -ne 1) { throw 'Expected one partition' }
    $part=$parts[0]
    $disk=Get-Disk -Number $part.DiskIndex
    $volume=Get-Volume -DriveLetter $Drive[0]
    if ($logical.VolumeName -cne 'TESTING' -or $logical.FileSystem -cne 'FAT32' -or
        $logical.VolumeSerialNumber -ine $ExpectedSerial -or $disk.BusType -ne 'USB' -or
        $disk.SerialNumber.Trim() -cne $ExpectedDeviceSerial -or $disk.IsBoot -or $disk.IsSystem -or
        $part.BootPartition -or $volume.AllocationUnitSize -ne 65536) { throw 'USB identity/64KiB geometry mismatch' }
    [ordered]@{Timestamp=(Get-Date -Format o); Drive=$Drive; Serial=$logical.VolumeSerialNumber;
        DeviceSerial=$disk.SerialNumber.Trim(); Disk=$disk.Number; PartitionBytes=$part.Size;
        PartitionOffset=$part.StartingOffset; ClusterBytes=$volume.AllocationUnitSize}
}
function Run-Check([string]$Name,[string]$Program,[string[]]$Arguments) {
    & $Program @Arguments 2>&1 | Set-Content -LiteralPath (Join-Path $evidence "$Name.txt")
    $code=$LASTEXITCODE
    "exit=$code" | Add-Content -LiteralPath (Join-Path $evidence "$Name.txt")
    if ($code -ne 0) { throw "$Name failed ($code); original output and fixtures retained" }
    Write-Output "PASS $Name"
}
$identity=Assert-Identity
New-Item -ItemType Directory -Path $evidence | Out-Null
$identity | ConvertTo-Json | Set-Content (Join-Path $evidence 'identity.json')
Run-Check 'chkdsk-before' 'chkdsk' @($Drive)
Run-Check 'read-before' (Join-Path $repo 'usbcheck.exe') @($Drive)
$destination=Join-Path "$Drive\" ('Native-64KiB-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $destination | Out-Null
function Pattern([int]$Length,[int]$Seed) {
    $bytes=[byte[]]::new($Length)
    for ($i=0;$i -lt $Length;$i++) { $bytes[$i]=($i*29+($i -shr 8)+$Seed) -band 255 }
    return ,$bytes
}
$expected=[Collections.Generic.List[object]]::new()
function Save-Fixture([string]$Name,[byte[]]$Bytes) {
    $path=Join-Path $destination $Name
    [IO.File]::WriteAllBytes($path,$Bytes)
    $expected.Add([pscustomobject]@{Name=$Name; Bytes=$Bytes.Length; SHA256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes))})
}
foreach ($size in @(0,1,511,512,513,32767,32768,32769,65535,65536,65537,131071,131072,131073,1048577)) {
    Save-Fixture "Boundary-$size.bin" (Pattern $size ($size -band 255))
}
# Occupy both neighbors before appending to the first file. Each initial write
# closes its handle so its length is exactly one cluster before the blockers.
$fragment=Pattern 196609 93
Save-Fixture 'Fragmented Ω.bin' $fragment[0..65535]
Save-Fixture 'Blocker-B.bin' (Pattern 65536 37)
Save-Fixture 'Blocker-C.bin' (Pattern 65536 71)
$stream=[IO.File]::Open((Join-Path $destination 'Fragmented Ω.bin'),[IO.FileMode]::Append)
try { $stream.Write($fragment,65536,$fragment.Length-65536); $stream.Flush($true) } finally { $stream.Dispose() }
$entry=$expected | Where-Object Name -eq 'Fragmented Ω.bin'
$entry.Bytes=$fragment.Length; $entry.SHA256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($fragment))
[ordered]@{Destination=$destination; Files=$expected} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $evidence 'native-fixtures.json')
Run-Check 'native-fixture-read' (Join-Path $repo 'usbcheck.exe') @($Drive)
$null=Assert-Identity
Run-Check 'write' (Join-Path $repo 'usbcheck.exe') @($Drive,'--write-test')
$null=Assert-Identity
Run-Check 'directory' (Join-Path $repo 'usbcheck.exe') @($Drive,'--directory-test')
Run-Check 'read-after' (Join-Path $repo 'usbcheck.exe') @($Drive)
Run-Check 'chkdsk-after' 'chkdsk' @($Drive)
$comparisons=@(foreach ($item in $expected) {
    $path=Join-Path $destination $item.Name
    $actual=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    [pscustomobject]@{Name=$item.Name; Bytes=(Get-Item -LiteralPath $path).Length;
        ExpectedSHA256=$item.SHA256; ActualSHA256=$actual; Match=($item.SHA256 -ceq $actual -and (Get-Item -LiteralPath $path).Length -eq $item.Bytes)}
})
$comparisons | ConvertTo-Json | Set-Content (Join-Path $evidence 'native-fixtures-after.json')
if (@($comparisons | Where-Object { -not $_.Match }).Count) { throw 'Native fixture hash changed' }
[ordered]@{Destination=$destination; Files=$comparisons.Count; Bytes=($expected | Measure-Object Bytes -Sum).Sum;
    NativeSHA256Matches=$comparisons.Count; Result='PASS'; Fragmentation='Must be measured independently from FAT chains'} |
    ConvertTo-Json | Set-Content (Join-Path $evidence 'result.json')
Get-Content (Join-Path $evidence 'result.json')
