cmake_minimum_required(VERSION 3.25)
project(vocatron LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBHACKRF REQUIRED IMPORTED_TARGET libhackrf)
pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
# Station screen: layout, buttons, and mouse clicks in the terminal.
find_package(ftxui 6 REQUIRED)

# C, not C++. miniaudio's PulseAudio code does not compile as C++ when it
# calls libpulse directly (enum conversions).
add_library(miniaudio STATIC src/core/miniaudio_impl.c)
target_include_directories(miniaudio PUBLIC src)
target_include_directories(miniaudio SYSTEM PUBLIC third_party)
target_compile_options(miniaudio PRIVATE -w)
target_link_libraries(miniaudio PUBLIC ${CMAKE_DL_LIBS})

add_executable(vocatron
               src/cli/main.cpp
               src/cli/tui.cpp
               src/core/capture.cpp
               src/core/channels.cpp
               src/core/dsp.cpp
               src/core/rds.cpp
               src/core/listen.cpp
               src/core/rx.cpp
               src/core/scan.cpp
               src/core/saved.cpp
               src/core/squelch.cpp
               src/core/playback.cpp)
target_include_directories(vocatron PRIVATE src)
target_include_directories(vocatron SYSTEM PRIVATE third_party)
target_link_libraries(vocatron PRIVATE PkgConfig::LIBHACKRF PkgConfig::FFTW3F Threads::Threads liquid
                                      miniaudio ftxui::component ftxui::dom ftxui::screen)

add_executable(vocatron_tests
               tests/ring_test.cpp
               tests/wfm_test.cpp
               tests/am_test.cpp
               tests/nfm_test.cpp
               tests/channels_test.cpp
               tests/playback_test.cpp
               tests/squelch_test.cpp
               tests/tui_test.cpp
               tests/level_test.cpp
               tests/rds_test.cpp
               tests/saved_test.cpp
               tests/search_test.cpp
               src/core/channels.cpp
               src/core/dsp.cpp
               src/core/rds.cpp
               src/core/saved.cpp
               src/core/scan.cpp
               src/core/rx.cpp
               src/core/playback.cpp
               src/core/squelch.cpp)
target_include_directories(vocatron_tests PRIVATE src)
target_include_directories(vocatron_tests SYSTEM PRIVATE third_party)
target_link_libraries(vocatron_tests PRIVATE PkgConfig::LIBHACKRF PkgConfig::FFTW3F Threads::Threads
                                             liquid miniaudio)

enable_testing()
add_test(NAME ring COMMAND vocatron_tests)
