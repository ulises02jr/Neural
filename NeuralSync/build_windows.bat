@echo off
REM ============================================================
REM  NeuralWorship - build para Windows
REM  Requiere: Python 3.11+  y  pip install -r requirements-windows.txt
REM ============================================================
setlocal

echo === 1) Verificando config.json ===
if not exist "config.json" (
  echo  * No hay config.json. Copio la plantilla...
  copy /Y "config.windows.example.json" "config.json" >nul
  echo.
  echo  ****************************************************************
  echo  *  IMPORTANTE: abri config.json y pega el TOKEN del VPS,       *
  echo  *  y confirma que "iac_port" coincide con tu puerto loopMIDI.  *
  echo  ****************************************************************
  echo.
  pause
)

echo === 3) Empaquetando con PyInstaller ===
pyinstaller --noconfirm NeuralWorship.spec
if errorlevel 1 (
  echo  ! Fallo el empaquetado.
  pause
  exit /b 1
)

echo.
echo === LISTO ===
echo La app quedo en:   dist\NeuralWorship\NeuralWorship.exe
echo Podes comprimir la carpeta  dist\NeuralWorship  y distribuirla,
echo o armar un instalador con Inno Setup (ver README_WINDOWS.md).
echo.
pause
endlocal
