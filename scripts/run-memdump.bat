@echo off
REM Runs memdump-diff.ps1 and calls the export function with fixed parameters.
REM No arguments needed - edit the values below to change target/address/size/output.
REM Uses Export-MemoryDumpByName so no PID has to be supplied or discovered manually.

setlocal

set "PS1_PATH=D:\VS_Workspace\MemAlloc\scripts\memdump-diff.ps1"
set "PROCESS_NAME=MemAlloc.exe"
set "ADDRESS=MemAlloc!mem_heap"
set "SIZE_BYTES=256"
set "OUT_FILE=D:\VS_Workspace\MemAlloc\dump\run.bin"

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
    ". '%PS1_PATH%'; Export-MemoryDumpByName -ProcessName '%PROCESS_NAME%' -Address '%ADDRESS%' -SizeBytes %SIZE_BYTES% -OutFile '%OUT_FILE%' -AlsoExportExcel"

endlocal
