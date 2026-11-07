@echo off
rem ===========================================================================
rem  TeleFiles - compilazione Release x64 (statica) + pacchetto portable
rem  Basta un doppio clic. Trova da solo Visual Studio (2022 o 2026) e vcpkg.
rem  La prima volta vcpkg compila TDLib e OpenSSL: 20-40 minuti.
rem ===========================================================================
setlocal EnableExtensions
title TeleFiles - compilazione
pushd "%~dp0"
set "VER=0.2.0"

echo.
echo  TeleFiles %VER% - compilazione
echo  ------------------------------
echo.

rem ---- 1. Visual Studio (compilatore C++, CMake)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :no_vs
echo  [ok] Visual Studio: %VSDIR%
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 goto :no_vs
where cmake >nul 2>nul
if errorlevel 1 goto :no_cmake

rem ---- 2. vcpkg: VCPKG_ROOT, poi C:\vcpkg, poi quello incluso in Visual Studio
if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\vcpkg.exe" goto :have_vcpkg
if exist "C:\vcpkg\vcpkg.exe" (
  set "VCPKG_ROOT=C:\vcpkg"
  goto :have_vcpkg
)
if exist "%VSDIR%\VC\vcpkg\vcpkg.exe" (
  set "VCPKG_ROOT=%VSDIR%\VC\vcpkg"
  goto :have_vcpkg
)
goto :no_vcpkg

:have_vcpkg
echo  [ok] vcpkg: %VCPKG_ROOT%
echo.

rem ---- 3. configurazione e compilazione
rem Ninja (incluso in Visual Studio) usa il compilatore caricato da VsDevCmd: funziona con
rem Visual Studio 2022 (17) e 2026 (18). Se manca, si usa il generatore di Visual Studio.
set "GEN=Ninja"
where ninja >nul 2>nul
if errorlevel 1 set "GEN=Visual Studio 17 2022"
if "%GEN%"=="Visual Studio 17 2022" if not "%VSDIR:\18\=%"=="%VSDIR%" set "GEN=Visual Studio 18 2026"
echo  [ok] Generatore: %GEN%

rem una cartella build creata con un altro generatore va ricreata
if exist "build\CMakeCache.txt" (
  findstr /c:"CMAKE_GENERATOR:INTERNAL=%GEN%" "build\CMakeCache.txt" >nul || rmdir /s /q build
)
echo.
echo  Configurazione (la prima volta scarica e compila TDLib: abbi pazienza)...
if "%GEN%"=="Ninja" (
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_INSTALL_OPTIONS=--clean-after-build
) else (
  cmake -S . -B build -G "%GEN%" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_INSTALL_OPTIONS=--clean-after-build
)
if errorlevel 1 goto :fail
echo.
echo  Compilazione...
cmake --build build --config Release
if errorlevel 1 goto :fail
set "EXE=build\TeleFiles.exe"
if exist "build\Release\TeleFiles.exe" set "EXE=build\Release\TeleFiles.exe"

rem ---- 4. pacchetto portable: stesso exe + file marcatore "TeleFiles.portable"
set "OUT=dist\TeleFiles-%VER%-portable"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"
copy /y "%EXE%" "%OUT%\" >nul
if errorlevel 1 goto :fail
copy /y "api.ini.example" "%OUT%\" >nul
copy /y "portable\LEGGIMI.txt" "%OUT%\" >nul
copy /y "portable\TeleFiles.portable" "%OUT%\" >nul
if exist "api.ini" copy /y "api.ini" "%OUT%\" >nul
if exist "dist\TeleFiles-%VER%-portable.zip" del "dist\TeleFiles-%VER%-portable.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\*' -DestinationPath 'dist\TeleFiles-%VER%-portable.zip'"
if errorlevel 1 goto :fail

echo.
echo  ==============================================================
echo   Fatto!
echo    dist\TeleFiles.exe                  versione normale
echo    dist\TeleFiles-%VER%-portable.zip    versione portable
echo  ==============================================================
echo.
rem copia l'exe fuori da build, così la cartella build si può cancellare
copy /y "%EXE%" "dist\TeleFiles.exe" >nul
echo  La cartella "build" contiene solo file temporanei di compilazione (molti GB).
echo  vcpkg tiene una copia compressa di TDLib nella sua cache: se la cancelli,
echo  la prossima compilazione la ripristina in pochi minuti.
choice /c SN /m "  Cancello ora la cartella build"
if errorlevel 2 goto :done_clean
rmdir /s /q build
echo  [ok] Cartella build cancellata.
:done_clean
explorer "dist"
goto :end

rem ---------------------------------------------------------------- errori
:no_vs
echo  [errore] Visual Studio 2022 o 2026 con il carico di lavoro
echo           "Sviluppo di applicazioni desktop con C++" non trovato.
echo           Installalo da https://visualstudio.microsoft.com/it/downloads/
echo           (va bene anche la Community gratuita) e riprova.
goto :end

:no_cmake
echo  [errore] CMake non trovato. Nel programma di installazione di Visual Studio
echo           aggiungi il componente "Strumenti CMake C++ per Windows".
goto :end

:no_vcpkg
echo  [errore] vcpkg non trovato.
echo.
echo  Posso installarlo ora in C:\vcpkg (serve Git, circa 1 minuto).
choice /c SN /m "  Installo vcpkg in C:\vcpkg"
if errorlevel 2 goto :vcpkg_manual
where git >nul 2>nul
if errorlevel 1 (
  echo  [errore] Git non trovato: installalo da https://git-scm.com e riprova.
  goto :end
)
git clone https://github.com/microsoft/vcpkg C:\vcpkg
if errorlevel 1 goto :fail
call C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
if errorlevel 1 goto :fail
set "VCPKG_ROOT=C:\vcpkg"
goto :have_vcpkg

:vcpkg_manual
echo  Installa vcpkg a mano (https://github.com/microsoft/vcpkg) in C:\vcpkg
echo  oppure imposta la variabile VCPKG_ROOT, poi rilancia build.bat.
goto :end

:fail
echo.
echo  [errore] La compilazione si e' fermata: guarda il messaggio qui sopra.
echo           Se vuoi aiuto, copia le ultime righe e incollale nella chat.

:end
echo.
popd
pause
endlocal
