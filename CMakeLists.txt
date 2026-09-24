cmake_minimum_required(VERSION 3.20)
project(fastrdp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(FREERDP REQUIRED IMPORTED_TARGET freerdp3 freerdp-client3 winpr3)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(Qt6Keychain REQUIRED)

add_executable(fastrdp
  src/main.cpp
  src/app.cpp
  src/rdp_session.cpp
  src/gfx_pipeline.cpp
  src/h264_decoder.cpp
  src/renderer.cpp
  src/gl_util.cpp
  src/keymap.cpp
  src/launcher/launcher.cpp
  src/launcher/main_window.cpp
  src/launcher/dialogs.cpp
  src/launcher/bookmark.cpp
  src/launcher/secrets.cpp
)
target_include_directories(fastrdp PRIVATE src)

target_compile_options(fastrdp PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_link_libraries(fastrdp PRIVATE
  PkgConfig::FREERDP
  PkgConfig::SDL3
  PkgConfig::EPOXY
  PkgConfig::FFMPEG
  PkgConfig::LIBDRM
  Threads::Threads
  Qt6::Widgets
  Qt6Keychain::Qt6Keychain
)


install(TARGETS fastrdp RUNTIME DESTINATION bin)
install(FILES packaging/fastrdp.desktop DESTINATION share/applications)
