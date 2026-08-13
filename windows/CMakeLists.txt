cmake_minimum_required(VERSION 3.16)
project(file-cleaner-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

if(NOT WIN32)
  message(WARNING
    "This target builds the Windows edition of Simple File Cleaner. "
    "It uses <windows.h>/Shell32 APIs and will not compile for a "
    "non-Windows target. Build it either on Windows (MSVC/MinGW) or by "
    "cross-compiling with a MinGW-w64 toolchain file, e.g.:\n"
    "  cmake -S windows -B build-win -G Ninja \\\n"
    "        -DCMAKE_TOOLCHAIN_FILE=windows/toolchain-mingw.cmake")
endif()

add_executable(file-cleaner-cli
    src/main.cpp
    src/common.cpp
    src/settings.cpp
    src/scan.cpp
    src/delete_engine.cpp
)

target_include_directories(file-cleaner-cli PRIVATE
    src
    src/third_party
)

if(WIN32)
  target_link_libraries(file-cleaner-cli PRIVATE shell32 ole32 uuid)
endif()

install(TARGETS file-cleaner-cli RUNTIME DESTINATION bin)
