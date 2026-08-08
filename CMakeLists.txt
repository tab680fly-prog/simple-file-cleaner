cmake_minimum_required(VERSION 3.16)
project(file-cleaner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)

add_executable(file-cleaner
    src/main.cpp
    src/common.cpp
    src/settings.cpp
    src/history.cpp
    src/scan.cpp
    src/scan_ring.cpp
    src/scan_page.cpp
    src/file_row.cpp
    src/settings_window.cpp
    src/main_window.cpp
    src/delete_engine.cpp
)

target_include_directories(file-cleaner PRIVATE
    src
    src/third_party
)

target_link_libraries(file-cleaner PRIVATE PkgConfig::GTK4 PkgConfig::ADWAITA Threads::Threads)

install(TARGETS file-cleaner RUNTIME DESTINATION bin)
install(FILES io.github.filecleaner.desktop DESTINATION ${CMAKE_INSTALL_PREFIX}/share/applications)
install(FILES io.github.filecleaner.svg
        DESTINATION ${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/scalable/apps)
install(FILES io.github.filecleaner.metainfo.xml DESTINATION ${CMAKE_INSTALL_PREFIX}/share/metainfo)
