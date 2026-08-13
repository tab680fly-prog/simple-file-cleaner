# MinGW-w64 cross-compilation toolchain for building file-cleaner-cli.exe
# from Linux/macOS. Install the compiler first, e.g. on Debian/Ubuntu:
#   sudo apt install g++-mingw-w64-x86-64
#
# Then build with:
#   cmake -S windows -B build-win -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=windows/toolchain-mingw.cmake
#   cmake --build build-win

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

find_program(CMAKE_C_COMPILER NAMES ${TOOLCHAIN_PREFIX}-gcc)
find_program(CMAKE_CXX_COMPILER NAMES ${TOOLCHAIN_PREFIX}-g++)
find_program(CMAKE_RC_COMPILER NAMES ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Statically link the MinGW runtime so file-cleaner-cli.exe doesn't need
# libstdc++/libgcc/libwinpthread DLLs alongside it.
set(CMAKE_EXE_LINKER_FLAGS "-static -static-libgcc -static-libstdc++")
