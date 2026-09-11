cmake_minimum_required(VERSION 3.20)
project(dabcore VERSION 3.0.0 LANGUAGES C CXX)

# DAB Classic – Empfangskern (Qt-frei).
# Build: MSYS2 UCRT64 (GCC, Ninja), siehe ../tools/build-core.ps1
#
#   libdabcore  – statische Bibliothek: Device -> OFDM -> FIC/MSC -> Audio/PAD/MOT/EPG
#   dabcored    – Prozess: JSON-Zeilen-Protokoll auf stdin/stdout + Headless-CLI

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(DABCORE_AUDIO_PORTAUDIO "Audio-Ausgabe per PortAudio im Kern" ON)
option(DABCORE_FDK_AAC_RUNTIME "FDK-AAC zur Laufzeit nachladbar (faad2 bleibt Standard)" ON)
option(DABCORE_BUILD_TESTS "Replay-Tests (ctest)" ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
pkg_check_modules(FAAD2 IMPORTED_TARGET faad2)
if(DABCORE_AUDIO_PORTAUDIO)
    pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
endif()
find_package(nlohmann_json 3.11 REQUIRED)
find_package(ZLIB REQUIRED)

# --- libdabcore ------------------------------------------------------------
file(GLOB_RECURSE DABCORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/libdabcore/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/libdabcore/src/*.c)

add_library(dabcore STATIC ${DABCORE_SOURCES})
# Die portierten Qt-DAB-Quellen verwenden flache Includes ("dab-constants.h");
# deshalb alle Teilverzeichnisse von src/ in den privaten Include-Pfad.
set(DABCORE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/libdabcore/src)
target_include_directories(dabcore
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/libdabcore/include
    PRIVATE ${DABCORE_SRC}
            ${DABCORE_SRC}/device
            ${DABCORE_SRC}/frontend
            ${DABCORE_SRC}/fic
            ${DABCORE_SRC}/dsp
            ${DABCORE_SRC}/dsp/viterbi
            ${DABCORE_SRC}/dsp/protection
            ${DABCORE_SRC}/support)
target_compile_definitions(dabcore PUBLIC
    __FFTW3__
    __ARCH_X86__           # Viterbi AVX2/SSE Runtime-Dispatch
    DABCORE_VERSION="${PROJECT_VERSION}")
target_link_libraries(dabcore PUBLIC
    PkgConfig::FFTW3F
    nlohmann_json::nlohmann_json
    ZLIB::ZLIB)
if(FAAD2_FOUND)
    target_compile_definitions(dabcore PUBLIC __WITH_FAAD__)
    target_link_libraries(dabcore PUBLIC PkgConfig::FAAD2)
endif()
if(DABCORE_AUDIO_PORTAUDIO)
    target_compile_definitions(dabcore PUBLIC DABCORE_AUDIO_PORTAUDIO)
    target_link_libraries(dabcore PUBLIC PkgConfig::PORTAUDIO)
endif()
if(DABCORE_FDK_AAC_RUNTIME)
    target_compile_definitions(dabcore PUBLIC DABCORE_FDK_AAC_RUNTIME)
endif()
# Viterbi-Kern bekommt AVX2/SSE4.1 (Dispatch zur Laufzeit, siehe v1 OPT-9.1)
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/libdabcore/src/dsp/viterbi/viterbi.cpp
    PROPERTIES COMPILE_OPTIONS "-mavx2;-msse4.1")

# --- dabcored --------------------------------------------------------------
add_executable(dabcored dabcored/main.cpp)
target_link_libraries(dabcored PRIVATE dabcore)
# Laufzeit-DLLs von MinGW nicht statisch: sie liegen mit im core/-Ordner.

# --- Tests -----------------------------------------------------------------
if(DABCORE_BUILD_TESTS)
    enable_testing()
    add_test(NAME dabcored_ready
        COMMAND ${CMAKE_COMMAND} -E env DABCORED=$<TARGET_FILE:dabcored>
                powershell -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-ready.ps1)
    # Replay am 60-s-Referenzmitschnitt (SKIP, wenn die Datei fehlt)
    add_test(NAME dabcored_replay
        COMMAND ${CMAKE_COMMAND} -E env DABCORED=$<TARGET_FILE:dabcored>
                powershell -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-replay.ps1)
endif()
