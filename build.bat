@echo off
REM build.bat            - build overlay.exe (the whole app).
REM build.bat <src.cpp>  - build any other single-file tool next to it
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
pushd "%~dp0"
if "%~1"=="" (
  cl /nologo /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS ^
     /O2 /Oi /Ot /GL /Gw /Gy /GS- /fp:fast ^
     overlay.cpp /Fe:overlay.exe ^
     /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF user32.lib gdi32.lib winmm.lib >build.log 2>&1
) else (
  cl /nologo /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /O2 /Oi /GL "%~nx1" /Fe:"%~n1.exe" ^
     /link /LTCG user32.lib gdi32.lib winmm.lib >build.log 2>&1
)
set RC=%errorlevel%
type build.log
if exist overlay.obj del /q overlay.obj
if exist *.obj del /q *.obj
popd
exit /b %RC%
