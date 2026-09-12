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
            ${DABCORE_SRC}/device/librtlsdr   # rtl-sdr.h includes <rtl-sdr_export.h>
            ${DABCORE_SRC}/scan
            ${DABCORE_SRC}/frontend
            ${DABCORE_SRC}/fic
            ${DABCORE_SRC}/dsp
            ${DABCORE_SRC}/dsp/viterbi
            ${DABCORE_SRC}/dsp/protection
            ${DABCORE_SRC}/support
            ${DABCORE_SRC}/backend
            ${DABCORE_SRC}/backend/audio
            ${DABCORE_SRC}/backend/reed-solomon
            ${DABCORE_SRC}/backend/data
            ${DABCORE_SRC}/backend/data/epg
            ${DABCORE_SRC}/pad
            ${DABCORE_SRC}/audio)
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
# FDK-AAC: nur der Header (Typen) wird gebraucht, die DLL wird zur Laufzeit
# geladen (Entscheidung 20). Ohne Header entfaellt der FDK-Pfad.
if(DABCORE_FDK_AAC_RUNTIME)
    find_path(FDK_AAC_INCLUDE_DIR aacdecoder_lib.h PATH_SUFFIXES fdk-aac)
    if(FDK_AAC_INCLUDE_DIR)
        target_compile_definitions(dabcore PUBLIC DABCORE_FDK_AAC_RUNTIME)
        target_include_directories(dabcore PRIVATE ${FDK_AAC_INCLUDE_DIR})
    else()
        message(STATUS "aacdecoder_lib.h nicht gefunden – FDK-AAC-Laufzeitpfad aus")
    endif()
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
    # MSC-Pfad: Dienst Dlf -> WAV-Dump, DLS, Stats; shutdown bei laufendem Dienst
    add_test(NAME dabcored_audio
        COMMAND ${CMAKE_COMMAND} -E env DABCORED=$<TARGET_FILE:dabcored>
                powershell -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-audio.ps1)
    # Geraete ohne Hardware: open_device rtl_sdr ohne DLL/Stick -> device_error
    # binnen 2 s, Prozess laeuft weiter, shutdown funktioniert; set_channel/
    # set_gain/start_scan ohne Geraet stuerzen nicht ab
    add_test(NAME dabcored_device
        COMMAND ${CMAKE_COMMAND} -E env DABCORED=$<TARGET_FILE:dabcored>
                powershell -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-device.ps1)
    # IQ-Dump-Roundtrip: Datei -> start_iq_dump (.uff, int8) -> Replay findet Ensemble
    add_test(NAME dabcored_iqdump
        COMMAND ${CMAKE_COMMAND} -E env DABCORED=$<TARGET_FILE:dabcored>
                powershell -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-iqdump.ps1)
    # SPI/EPG: automatischer Hintergrunddienst am 8-min-Ausschnitt des
    # Warntag-Mitschnitts -> mot_object (PNG-Logos) und epg_object (XML)
    add_test(NAME dabcored_epg
        COMMAND ${CMAKE_COMMAND} -E env DABCORED=$<TARGET_FILE:dabcored>
                powershell -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-epg.ps1)
    # state_snapshot (running[] mit dls/codec, ensemble als [eid, name]),
    # audio_devices nach ready/get_state, set_scopes (spectrum + iq_samples
    # mit Rate und Groesse), stdin-EOF bei laufendem Dienst -> Ende < 1 s
    add_test(NAME dabcored_snapshot
        COMMAND ${CMAKE_COMMAND} -E env DABCORED=$<TARGET_FILE:dabcored>
                powershell -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-snapshot.ps1)
endif()
