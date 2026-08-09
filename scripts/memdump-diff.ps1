<#
.SYNOPSIS
  Export a memory region from a process already stopped under a debugger (e.g. VS),
  and/or diff two previously captured dumps.

.PREREQUISITE
  Requires cdb.exe from "Debugging Tools for Windows" (installed with the Windows SDK).
  Default path assumed below; override with -CdbPath if yours differs.

.NOTES
  Uses -pv (non-invasive attach): this only works while another debugger (VS) is
  already attached and the process is suspended at a breakpoint. cdb reads memory
  and detaches without taking over or resuming the process.
#>

function Add-TimestampToPath {
    # Prefixes a human-readable, zero-padded timestamp before the extension, e.g.
    # "C:\dumps\run.bin" -> "C:\dumps\20260806_143045123_run.bin"
    # Format: yyyyMMdd_HHmmssfff (year, month, day, hour, minute, second, millisecond).
    # Fixed-width and zero-padded, so sorting filenames by name also sorts them
    # chronologically - oldest first in ascending order, newest first if you sort
    # descending (most file browsers let you flip that with one click on the column).
    param([Parameter(Mandatory)][string]$Path)
    $dir  = Split-Path $Path -Parent
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($Path)
    $ext  = [System.IO.Path]::GetExtension($Path)
    $ts   = Get-Date -Format "yyyyMMdd_HHmmssfff"
    $name = "${ts}_${stem}${ext}"
    if ($dir) { return (Join-Path $dir $name) } else { return $name }
}

function Export-HexGridXlsx {
    <#
      Converts a raw .bin dump into a hex-editor-style grid in Excel:
      each row = one 16-byte line, labeled by its ABSOLUTE starting address
      (BaseAddress + row offset); columns 0-F = the byte at that position
      within the row (hex, 2 digits).
      Requires the ImportExcel module (Install-Module ImportExcel -Scope CurrentUser).
    #>
    param(
        [Parameter(Mandatory)][string]$BinFile,
        [Parameter(Mandatory)][string]$XlsxFile,
        [UInt64]$BaseAddress = 0   # absolute address of byte 0 in BinFile; rows are labeled from this
    )
    if (-not (Get-Module -ListAvailable -Name ImportExcel)) {
        throw "ImportExcel module not found. Install once with: Install-Module ImportExcel -Scope CurrentUser"
    }
    Import-Module ImportExcel -ErrorAction Stop

    $bytes = [System.IO.File]::ReadAllBytes($BinFile)
    $rowCount = [Math]::Ceiling($bytes.Length / 16)

    $data = for ($r = 0; $r -lt $rowCount; $r++) {
        $rowAddr = $BaseAddress + [UInt64]($r * 16)
        $rowObj = [ordered]@{ Address = "0x{0:x16}" -f $rowAddr }
        for ($c = 0; $c -lt 16; $c++) {
            $idx = $r * 16 + $c
            $colName = "{0:X2}" -f $c
            $rowObj[$colName] = if ($idx -lt $bytes.Length) { "{0:X2}" -f $bytes[$idx] } else { "" }
        }
        [PSCustomObject]$rowObj
    }

    # Address + 16 hex-byte columns = Excel columns A through Q. Right-align
    # everything (header row included) so the hex bytes line up like a real
    # hex editor instead of ragging left as text.
    $rightAlign = New-ExcelStyle -Range ("A1:Q{0}" -f ($rowCount + 1)) -HorizontalAlignment Right

    # Export-Excel auto-converts numeric-looking strings to real numbers,
    # which silently strips the leading zero ("00" -> 0, "05" -> 5). Only the
    # purely-numeric byte values (00-09) are actually at risk - 0A-0F aren't
    # valid numbers so they'd survive anyway - but excluding every hex-byte
    # column (headers included, since "00".."09" are column names too) keeps
    # every column consistently text and consistently padded.
    $hexColumns = 0..15 | ForEach-Object { "{0:X2}" -f $_ }

    $data | Export-Excel -Path $XlsxFile -AutoSize -FreezeTopRowFirstColumn -ClearSheet -Style $rightAlign -NoNumberConversion $hexColumns
    Write-Host "Wrote hex grid ($rowCount rows, base 0x$('{0:x16}' -f $BaseAddress)) to $XlsxFile"
}

function Export-MemoryDump {
    param(
        [Parameter(Mandatory)][int]$ProcessId,
        [Parameter(Mandatory)][string]$Address,   # e.g. "0x00007ff6a1b23000" or "mymodule!myArray"
        [Parameter(Mandatory)][int]$SizeBytes,
        [Parameter(Mandatory)][string]$OutFile,   # treated as a base name/path; timestamp is auto-inserted
        [string]$CdbPath = "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe",
        [switch]$NoTimestamp,                     # opt out and use OutFile exactly as given
        [switch]$AlsoExportExcel                  # additionally write a hex-grid .xlsx alongside the .bin
    )
    if (-not (Test-Path $CdbPath)) {
        throw "cdb.exe not found at '$CdbPath'. Install Debugging Tools for Windows or pass -CdbPath."
    }
    $resolvedOutFile = if ($NoTimestamp) { $OutFile } else { Add-TimestampToPath -Path $OutFile }

    $outDir = Split-Path $resolvedOutFile -Parent
    if ($outDir -and -not (Test-Path $outDir)) {
        New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    }

    $sizeHex = "{0:x}" -f $SizeBytes

    # cdb's .writemem has been observed mangling quoted paths that contain spaces
    # (e.g. "D:\VS workspace\...") even though it echoes the correct-looking path back -
    # it silently writes to a garbled location instead. Work around this by having cdb
    # write to a plain, space-free temp path, then move the result into place ourselves.
    # Stage on the SAME drive as the real destination, in a space-free folder name.
    # C:\ (including %TEMP%) has been observed failing writes outright on this machine,
    # while the destination drive has worked throughout - so stage there instead.
    $destRoot = [System.IO.Path]::GetPathRoot($resolvedOutFile)
    $stagingDir = Join-Path $destRoot "_memdump_staging"
    if (-not (Test-Path $stagingDir)) {
        New-Item -ItemType Directory -Path $stagingDir -Force | Out-Null
    }
    $tempBin = Join-Path $stagingDir ([Guid]::NewGuid().ToString("N") + ".bin")

    $scriptFile = [System.IO.Path]::GetTempFileName()
    try {
        Set-Content -Path $scriptFile -Value "? $Address" -Encoding ASCII
        $tempBinForCdb = $tempBin -replace '\\', '/'
        Add-Content -Path $scriptFile -Value ".writemem `"$tempBinForCdb`" $Address L$sizeHex" -Encoding ASCII
        Add-Content -Path $scriptFile -Value "q" -Encoding ASCII

        $cdbOutput = & $CdbPath -pv -p $ProcessId -cf $scriptFile 2>&1
    } finally {
        Remove-Item -Path $scriptFile -ErrorAction SilentlyContinue
    }

    $resolvedAddress = [UInt64]0
    $evalLine = $cdbOutput | Where-Object { $_ -match 'Evaluate expression:' } | Select-Object -First 1
    if ($evalLine -and $evalLine -match '=\s*([0-9a-fA-F`]+)\s*$') {
        $hexRaw = $Matches[1] -replace '`', ''
        $resolvedAddress = [Convert]::ToUInt64($hexRaw, 16)
    }

    # Retry against the temp path (no spaces, so no reason for cdb's bug to bite here)
    $found = $false
    for ($i = 0; $i -lt 30; $i++) {
        if (Test-Path $tempBin) { $found = $true; break }
        Start-Sleep -Milliseconds 500
    }

    if (-not $found) {
        Write-Host "----- cdb output -----"
        $cdbOutput | ForEach-Object { Write-Host $_ }
        Write-Host "-----------------------"
        throw "Export failed - no output file produced even at the space-free temp path. Check PID/address/that the process is currently stopped."
    }

    Move-Item -Path $tempBin -Destination $resolvedOutFile -Force
    Write-Host "Wrote $((Get-Item $resolvedOutFile).Length) bytes to $resolvedOutFile (base address 0x$('{0:x16}' -f $resolvedAddress))"

    if ($AlsoExportExcel) {
        $xlsxPath = [System.IO.Path]::ChangeExtension($resolvedOutFile, ".xlsx")
        Export-HexGridXlsx -BinFile $resolvedOutFile -XlsxFile $xlsxPath -BaseAddress $resolvedAddress
    }

    return $resolvedOutFile
}

function Compare-MemoryDump {
    param(
        [Parameter(Mandatory)][string]$FileA,
        [Parameter(Mandatory)][string]$FileB
    )
    $a = [System.IO.File]::ReadAllBytes($FileA)
    $b = [System.IO.File]::ReadAllBytes($FileB)

    if ($a.Length -ne $b.Length) {
        Write-Warning "Size mismatch: $FileA = $($a.Length) bytes, $FileB = $($b.Length) bytes"
    }

    $len = [Math]::Min($a.Length, $b.Length)
    $diffCount = 0
    $firstDiffOffset = -1
    for ($i = 0; $i -lt $len; $i++) {
        if ($a[$i] -ne $b[$i]) {
            if ($firstDiffOffset -eq -1) { $firstDiffOffset = $i }
            $diffCount++
        }
    }

    if ($diffCount -eq 0) {
        Write-Host "No byte differences in the overlapping range ($len bytes)."
    } else {
        Write-Host "Differences: $diffCount byte(s) out of $len."
        Write-Host ("First diff at offset 0x{0:x} : {1:x2} -> {2:x2}" -f $firstDiffOffset, $a[$firstDiffOffset], $b[$firstDiffOffset])
    }
}

function Export-MemoryDumpByName {
    <#
      Same as Export-MemoryDump, but resolves the PID from an exe name instead of
      requiring the caller to pass -ProcessId. Lets you invoke with only static,
      known-in-advance arguments (name, address, size, outfile).
    #>
    param(
        [Parameter(Mandatory)][string]$ProcessName,   # e.g. "myapp" (no .exe) or "myapp.exe"
        [Parameter(Mandatory)][string]$Address,
        [Parameter(Mandatory)][int]$SizeBytes,
        [Parameter(Mandatory)][string]$OutFile,
        [string]$CdbPath = "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe",
        [switch]$NoTimestamp,
        [switch]$AlsoExportExcel
    )
    $baseName = $ProcessName -replace '\.exe$', ''
    $matches = @(Get-Process -Name $baseName -ErrorAction SilentlyContinue)

    if ($matches.Count -eq 0) {
        throw "No running process named '$baseName' was found."
    }
    if ($matches.Count -gt 1) {
        $idList = ($matches | ForEach-Object { "$($_.Id) (started $($_.StartTime))" }) -join "; "
        throw "Multiple '$baseName' processes are running: $idList. Use Export-MemoryDump with an explicit -ProcessId instead."
    }

    Export-MemoryDump -ProcessId $matches[0].Id -Address $Address -SizeBytes $SizeBytes -OutFile $OutFile -CdbPath $CdbPath -NoTimestamp:$NoTimestamp -AlsoExportExcel:$AlsoExportExcel
}

# --- Example usage ---
# Same -OutFile "base name" produces a distinct file each call - a millisecond
# timestamp is auto-inserted before the extension, so nothing gets overwritten.
#
# Export-MemoryDump -ProcessId 12345 -Address "0x000001f2a3b40000" -SizeBytes 256 -OutFile "C:\dumps\run.bin"
#   -> C:\dumps\run_20260806_142317456.bin
# ... let the process continue / hit the breakpoint again ...
# Export-MemoryDump -ProcessId 12345 -Address "0x000001f2a3b40000" -SizeBytes 256 -OutFile "C:\dumps\run.bin"
#   -> C:\dumps\run_20260806_142905112.bin
# Compare-MemoryDump -FileA "C:\dumps\run_20260806_142317456.bin" -FileB "C:\dumps\run_20260806_142905112.bin"
#
# One-liner using the exe name instead of a PID (fails loudly if 0 or 2+ matches):
# Export-MemoryDumpByName -ProcessName "myapp.exe" -Address "myapp!myArray" -SizeBytes 256 -OutFile "C:\dumps\run.bin"
#
# Opt out of timestamping if you want exact control over the filename yourself:
# Export-MemoryDump -ProcessId 12345 -Address "..." -SizeBytes 256 -OutFile "C:\dumps\run.bin" -NoTimestamp
