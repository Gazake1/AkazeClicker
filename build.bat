@echo off
REM ============================================================================
REM  build.bat - compila o Akaze com MinGW-w64 (g++)
REM  Requisitos: git + g++/windres/strip no PATH
REM              (ex: winget install BrechtSanders.WinLibs.POSIX.UCRT)
REM  Saida: Akaze.exe e Akaze-Admin.exe (mesmo programa, com manifesto de admin)
REM  O Dear ImGui e baixado em uma versao FIXA (commit abaixo) para o build ser reproduzivel.
REM ============================================================================
setlocal

set IMGUI_COMMIT=2859f6723c13de3a9661b8087fb7919fa3796813

where g++ >nul 2>nul
if errorlevel 1 (
  echo [ERRO] g++ nao encontrado no PATH.
  echo        Instale um compilador MinGW-w64 e abra um terminal novo.
  exit /b 1
)

REM --- baixa o Dear ImGui na versao fixa, se ainda nao tiver ------------------
if not exist imgui\imgui.cpp (
  echo [*] Baixando Dear ImGui %IMGUI_COMMIT% ...
  git init -q imgui || exit /b 1
  git -C imgui remote add origin https://github.com/ocornut/imgui.git || exit /b 1
  git -C imgui fetch -q --depth 1 origin %IMGUI_COMMIT% || (echo [ERRO] Falha ao baixar o Dear ImGui. & exit /b 1)
  git -C imgui checkout -q FETCH_HEAD || exit /b 1
)

echo [*] Gerando recursos (icone e versao)...
windres app.rc -O coff -o app.res.o
if errorlevel 1 (
  echo [ERRO] Falha ao gerar o recurso com o windres.
  exit /b 1
)

echo [*] Compilando...
g++ -std=c++17 -O2 -DNDEBUG -Wall -Wno-misleading-indentation ^
  -I imgui -I imgui\backends ^
  main.cpp ^
  imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp ^
  imgui\backends\imgui_impl_dx11.cpp imgui\backends\imgui_impl_win32.cpp ^
  app.res.o ^
  -o Akaze.exe ^
  -mwindows -static -static-libgcc -static-libstdc++ ^
  -ld3d11 -ldxgi -ld3dcompiler -lgdi32 -luser32 -limm32 -ldwmapi -lwinmm
if errorlevel 1 (
  echo [ERRO] Falha na compilacao.
  exit /b 1
)

strip --strip-all Akaze.exe
if errorlevel 1 (
  echo [ERRO] Falha ao remover os simbolos de depuracao.
  exit /b 1
)

echo [*] Gerando a versao Admin...
powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-admin.ps1 -Source Akaze.exe -Dest Akaze-Admin.exe -Manifest akaze.manifest
if errorlevel 1 (
  echo [ERRO] Falha ao gerar o Akaze-Admin.exe.
  exit /b 1
)

echo.
echo [OK] Gerados: Akaze.exe e Akaze-Admin.exe
endlocal
