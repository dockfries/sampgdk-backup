@echo off
rem Extracts open.mp server natives from the open.mp Pawn include directory.
rem
rem Usage: extract_all.bat <path-to-open.mp-include-dir>
rem   e.g. extract_all.bat "C:\path\to\open.mp\Server\qawno\include"
setlocal
cd /d D:\Work\sampgdk-backup\scripts

if "%~1"=="" (
  echo Usage: extract_all.bat ^<open.mp include dir^>
  exit /b 1
)

set "INC=%~1"
set OUT=D:\Work\sampgdk-backup\extracted_natives
if not exist %OUT% mkdir %OUT%

rem Only extract open.mp server natives (omp_*.inc and a_*.inc).
rem Pawn standard library includes (float.inc, string.inc, file.inc,
rem time.inc, core.inc, console.inc, datagram.inc, args.inc) are NOT
rem server natives and must be excluded.
for %%f in ("%INC%\omp_*.inc") do (
  echo === %%f ===
  python extract_omp_natives.py "%%f" "%OUT%\%%~nf.idl"
)
for %%f in ("%INC%\a_*.inc") do (
  echo === %%f ===
  python extract_omp_natives.py "%%f" "%OUT%\%%~nf.idl"
)

echo DONE
