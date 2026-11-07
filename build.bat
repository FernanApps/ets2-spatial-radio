@echo off
setlocal
cd /d "%~dp0"

set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Release

echo === ETS2 Cabin Radio Build ===
echo Config: %CONFIG%
echo.

rem Configurar CMake si no existe build/
if not exist "build\CMakeCache.txt" (
    echo Configurando CMake...
    cmake -B build -G "Visual Studio 17 2022" -A x64
    if errorlevel 1 (
        echo Error: cmake configure fallo
        exit /b 1
    )
)

rem Compilar
echo.
echo Compilando...
cmake --build build --config %CONFIG% --target ETS2CabinDSP bridge_test
if errorlevel 1 (
    echo Error: build fallo
    exit /b 1
)

echo.
echo === Build completado ===
echo   DLL:  build\%CONFIG%\ETS2CabinDSP.dll
echo   Test: build\%CONFIG%\bridge_test.exe
echo.
echo Para probar:
echo   1. Copia build\%CONFIG%\ETS2CabinDSP.dll a bin\win_x64\plugins\ de ETS2
echo   2. Corre: python tools\bridge_test.py
echo   3. Inicia ETS2 y enciende la radio
