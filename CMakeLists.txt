# ============================================================================
#  CMakeLists.txt - alternativa ao build.bat (Visual Studio / MinGW + CMake)
#  Baixa o Dear ImGui em uma versao FIXA via FetchContent.
#
#  Passos (no diretorio do projeto):
#    cmake -S . -B build
#    cmake --build build --config Release
#  Saida: build/Release/GNClicker.exe  (ou build/GNClicker.exe no MinGW)
#
#  Obs: este CMake gera so o GNClicker.exe normal. A versao Admin e criada pelo
#  tools/make-admin.ps1 (troca o manifesto do exe).
# ============================================================================
cmake_minimum_required(VERSION 3.14)
project(GNClicker LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        2859f6723c13de3a9661b8087fb7919fa3796813   # mesma versao do build.bat
)
FetchContent_MakeAvailable(imgui)

add_executable(GNClicker WIN32
  main.cpp
  app.rc
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(GNClicker PRIVATE
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(GNClicker PRIVATE
  d3d11 dxgi d3dcompiler gdi32 user32 imm32 dwmapi winmm
)

# MinGW: linkar runtime estatico para o exe rodar sem DLLs extras
if(MINGW)
  target_link_options(GNClicker PRIVATE -static -static-libgcc -static-libstdc++)
endif()
