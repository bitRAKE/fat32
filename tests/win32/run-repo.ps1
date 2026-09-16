# Export exact Git blobs, exercise the raw FAT32 library, then let Git itself
# verify raw-library and independent native readback. Requires PowerShell 7.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Z]:$')][string]$Drive,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{8}$')][string]$ExpectedSerial,
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$ExpectedDeviceSerial,
    [string]$Revision='HEAD',
    [Parameter(Mandatory)][string]$EvidenceDirectory
)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
function Invoke-GitText([string[]]$GitArguments) {
    $lines=& git -C $repo @GitArguments
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArguments -join ' ') failed ($LASTEXITCODE)" }
    return $lines
}
function Get-BlobID([string]$Path) {
    return (Invoke-GitText @('hash-object','--no-filters','--',$Path)).Trim()
}
function Export-Blob([string]$ObjectID,[string]$Path) {
    # Use bytes from the process stream; PowerShell text redirection would risk
    # newline/encoding conversion. cat-file applies no working-tree filters.
    $start=[Diagnostics.ProcessStartInfo]::new('git')
    $start.UseShellExecute=$false
    $start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true
    $start.RedirectStandardError=$true
    foreach ($argument in @('-C',$repo,'cat-file','blob',$ObjectID)) { $start.ArgumentList.Add($argument) }
    $process=[Diagnostics.Process]::new()
    $process.StartInfo=$start
    if (-not $process.Start()) { throw 'Cannot start git cat-file' }
    $errors=$process.StandardError.ReadToEndAsync()
    $output=[IO.File]::Open($Path,[IO.FileMode]::CreateNew)
    try { $process.StandardOutput.BaseStream.CopyTo($output) } finally { $output.Dispose() }
    $process.WaitForExit()
    $errorText=$errors.GetAwaiter().GetResult()
    $code=$process.ExitCode
    $process.Dispose()
    if ($code -ne 0) { throw "cat-file failed: $errorText" }
}
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 is required' }
$commit=(Invoke-GitText @('rev-parse','--verify',"$Revision^{commit}")).Trim()
$algorithm=(Invoke-GitText @('rev-parse','--show-object-format')).Trim()
if ($algorithm -ne 'sha1') { throw 'This manifest harness currently supports SHA-1 Git repositories' }
$logical=Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$Drive'"
$partitions=@(Get-CimAssociatedInstance -InputObject $logical -Association Win32_LogicalDiskToPartition)
if ($partitions.Count -ne 1) { throw 'Expected one partition for the test drive' }
$partition=$partitions[0]
$disk=Get-CimInstance Win32_DiskDrive -Filter "Index=$($partition.DiskIndex)"
if ($logical.VolumeName -cne 'TESTING' -or $logical.FileSystem -cne 'FAT32' -or
    $logical.VolumeSerialNumber -ine $ExpectedSerial -or $disk.InterfaceType -cne 'USB' -or
    $disk.SerialNumber.Trim() -cne $ExpectedDeviceSerial -or $partition.BootPartition) {
    throw 'Test volume/device identity does not match the expected USB'
}
$evidence=[IO.Path]::GetFullPath($EvidenceDirectory)
if ($evidence.StartsWith($Drive,[StringComparison]::OrdinalIgnoreCase)) { throw 'Keep evidence off the test drive' }
if ($evidence.StartsWith($repo+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
    & git -C $repo check-ignore -q -- (Join-Path $evidence 'identity.json')
    if ($LASTEXITCODE -ne 0) { throw 'Use an ignored output directory such as build\repo-test, or a path outside the repository' }
}
if (Test-Path -LiteralPath $evidence) { throw 'Use a new evidence directory; existing runs are preserved' }
New-Item -ItemType Directory -Path $evidence | Out-Null
$work=Join-Path $repo ('build\repo-'+[guid]::NewGuid().ToString('N'))
$blobs=Join-Path $work 'blobs'
$readback=Join-Path $work 'readback'
New-Item -ItemType Directory -Path $blobs,$readback | Out-Null
$identity=[ordered]@{
    Timestamp=(Get-Date -Format o); Commit=$commit; ObjectFormat=$algorithm
    Drive=$Drive; Label=$logical.VolumeName; VolumeSerial=$logical.VolumeSerialNumber
    Disk=$disk.Index; DeviceSerial=$disk.SerialNumber.Trim(); Model=$disk.Model
    PartitionBytes=$partition.Size; PartitionOffset=$partition.StartingOffset
    ExpectedSectorBytes=512; ExpectedClusterBytes=512; LocalWork=$work
}
$identity | ConvertTo-Json | Set-Content (Join-Path $evidence 'identity.json')
$items=@(foreach ($line in (Invoke-GitText @('-c','core.quotePath=false','ls-tree','-rl',$commit))) {
    if ($line -notmatch '^100644 blob ([0-9a-f]{40}) +([0-9]+)\t(.+)$') { throw "Unsupported tree entry: $line" }
    $objectID=$Matches[1]; $length=[uint32]$Matches[2]; $relative=$Matches[3]
    if ($relative.StartsWith('"') -or $relative -match '[\t\r\n\\:]' -or $length -gt 16MB) { throw "Unsupported blob path/size: $relative" }
    [pscustomobject]@{Path=$relative; Bytes=$length; Blob=$objectID}
})
$items=@($items | Sort-Object Bytes,Path)
if (-not $items.Count -or $items.Count -gt 4096) { throw 'Expected 1..4096 regular tracked files' }
$manifest=Join-Path $evidence 'manifest.tsv'
$records=foreach ($item in $items) {
    $blobPath=Join-Path $blobs $item.Blob
    if (-not (Test-Path -LiteralPath $blobPath)) { Export-Blob $item.Blob $blobPath }
    if ((Get-Item -LiteralPath $blobPath).Length -ne $item.Bytes -or (Get-BlobID $blobPath) -cne $item.Blob) {
        throw "Export does not match Git object: $($item.Path)"
    }
    "$($item.Blob)`t$($item.Bytes)`t$($item.Path)"
}
[IO.File]::WriteAllLines($manifest,[string[]]$records,[Text.UTF8Encoding]::new($false))
Write-Output "SOURCE commit=$commit files=$($items.Count); exported blobs verified with git hash-object --no-filters"
& (Join-Path $repo 'build\win32\usbcheck.exe') $Drive 2>&1 | Tee-Object (Join-Path $evidence 'usb-before.txt')
if ($LASTEXITCODE -ne 0) { throw 'Preflight raw/native comparison failed' }
& chkdsk $Drive 2>&1 | Tee-Object (Join-Path $evidence 'chkdsk-before.txt')
$beforeExit=$LASTEXITCODE
"chkdsk_exit=$beforeExit" | Add-Content (Join-Path $evidence 'chkdsk-before.txt')
if ($beforeExit -ne 0) { throw 'Preflight CHKDSK failed' }
$sessionFile=Join-Path $evidence 'session-name.txt'
& (Join-Path $repo 'build\win32\repocheck.exe') $Drive $ExpectedSerial $manifest $blobs $readback $sessionFile 2>&1 |
    Tee-Object (Join-Path $evidence 'operations.txt')
$testExit=$LASTEXITCODE
"repocheck_exit=$testExit" | Add-Content (Join-Path $evidence 'operations.txt')
if ($testExit -ne 0) { throw "Raw fragmentation test failed ($testExit); artifacts preserved" }
$session=(Get-Content -LiteralPath $sessionFile -Raw).Trim()
if ($session -notmatch '^FAT32-repo-[0-9A-F]{16}-[0-9]+$') { throw 'Invalid generated session name' }
$destination=Join-Path "$Drive\" $session
$results=@(for ($index=0;$index -lt $items.Count;$index++) {
    $item=$items[$index]
    $rawPath=Join-Path $readback ('{0:d4}.bin' -f $index)
    $nativePath=Join-Path $destination $item.Path.Replace('/','\')
    $rawID=Get-BlobID $rawPath
    $nativeID=Get-BlobID $nativePath
    $rawBytes=(Get-Item -LiteralPath $rawPath).Length
    $nativeBytes=(Get-Item -Force -LiteralPath $nativePath).Length
    $matched=$rawID -ceq $item.Blob -and $nativeID -ceq $item.Blob -and $rawBytes -eq $item.Bytes -and $nativeBytes -eq $item.Bytes
    [pscustomobject]@{Index=$index; Path=$item.Path; Bytes=$item.Bytes; ExpectedGitBlob=$item.Blob; RawGitBlob=$rawID; NativeGitBlob=$nativeID; Match=$matched}
})
$results | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $evidence 'git-comparisons.json')
if (@($results | Where-Object { -not $_.Match }).Count) { throw 'Git blob comparison failed; see git-comparisons.json' }
$nativeFiles=@(Get-ChildItem -LiteralPath $destination -File -Recurse -Force)
if ($nativeFiles.Count -ne $items.Count) { throw 'Native final file count differs from Git tree' }
Copy-Item -LiteralPath (Join-Path $readback 'chains.tsv'),(Join-Path $readback 'summary.tsv') -Destination $evidence
$chains=Import-Csv -LiteralPath (Join-Path $evidence 'chains.tsv') -Delimiter "`t"
$owners=[Collections.Generic.HashSet[uint32]]::new()
foreach ($chain in $chains) {
    foreach ($cluster in ($chain.Chain -split ',' | Where-Object { $_ })) {
        if (-not $owners.Add([uint32]$cluster)) { throw "Shared cluster detected in repository files: $cluster" }
    }
}
& chkdsk $Drive 2>&1 | Tee-Object (Join-Path $evidence 'chkdsk-after.txt')
$afterExit=$LASTEXITCODE
"chkdsk_exit=$afterExit" | Add-Content (Join-Path $evidence 'chkdsk-after.txt')
if ($afterExit -ne 0) { throw 'Post-test CHKDSK failed' }
& (Join-Path $repo 'build\win32\usbcheck.exe') $Drive 2>&1 | Tee-Object (Join-Path $evidence 'usb-after.txt')
if ($LASTEXITCODE -ne 0) { throw 'Final raw/native comparison failed' }
$summary=Import-Csv -LiteralPath (Join-Path $evidence 'summary.tsv') -Delimiter "`t"
$final=[ordered]@{
    Timestamp=(Get-Date -Format o); Commit=$commit; Destination=$destination
    Files=$items.Count; Bytes=($items | Measure-Object Bytes -Sum).Sum
    RawGitMatches=$results.Count; NativeGitMatches=$results.Count
    Passes=[int]$summary.Passes; Writes=[int]$summary.Writes; Deletes=[int]$summary.Deletes
    Commits=[int]$summary.Commits; FragmentedFiles=[int]$summary.Fragmented
    DistinctFileClusters=$owners.Count; ChkdskBeforeExit=$beforeExit; ChkdskAfterExit=$afterExit
}
$final | ConvertTo-Json | Set-Content (Join-Path $evidence 'result.json')
$final | ConvertTo-Json
Write-Output "PASS: all $($items.Count) files match their committed Git blobs through raw and native reads; retained at $destination"
