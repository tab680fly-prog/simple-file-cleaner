# Cross-compiles for 64-bit Windows with llvm-mingw against an MSYS2 UCRT64
# sysroot. Used by build-windows.sh; LLVM_MINGW must point at the toolchain.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_tc "$ENV{LLVM_MINGW}")
set(CMAKE_C_COMPILER "${_tc}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${_tc}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${_tc}/bin/x86_64-w64-mingw32-windres")
