cmake_minimum_required(VERSION 3.20)
project(TeleFiles VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(TELEFILES_PORTABLE "Exe sempre portable (altrimenti basta il file TeleFiles.portable accanto all'exe)" OFF)
set(TELEFILES_API_ID "" CACHE STRING "api_id di my.telegram.org (facoltativo: in alternativa api.ini)")
set(TELEFILES_API_HASH "" CACHE STRING "api_hash di my.telegram.org (facoltativo)")

find_package(nlohmann_json CONFIG REQUIRED)

set(CORE_SOURCES
    src/core/Engine.cpp
    src/core/Sha256.cpp
    src/core/Split.cpp
    src/core/TdClient.cpp)

if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive- /wd4100)
    # runtime statico, come le altre build release
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(WIN32)
    find_package(Td CONFIG REQUIRED)
    add_executable(TeleFiles WIN32
        ${CORE_SOURCES}
        src/app/main.cpp
        src/app/MainWindow.cpp
        src/app/Icons.cpp
        third_party/qrcodegen/qrcodegen.cpp
        res/TeleFiles.rc)
    target_compile_definitions(TeleFiles PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    if(TELEFILES_PORTABLE)
        target_compile_definitions(TeleFiles PRIVATE TF_PORTABLE)
    endif()
    if(TELEFILES_API_ID AND TELEFILES_API_HASH)
        target_compile_definitions(TeleFiles PRIVATE TF_API_ID=${TELEFILES_API_ID} TF_API_HASH="${TELEFILES_API_HASH}")
    endif()
    if(TARGET Td::TdJsonStatic)
        set(TD_TARGET Td::TdJsonStatic)
    else()
        set(TD_TARGET Td::TdJson)
    endif()
    target_link_libraries(TeleFiles PRIVATE ${TD_TARGET} nlohmann_json::nlohmann_json
        d2d1 dwrite windowscodecs dwmapi comctl32 shell32 shlwapi ole32 uxtheme)
    if(MSVC)
        # il manifest è già nelle risorse
        target_link_options(TeleFiles PRIVATE /MANIFEST:NO)
    endif()
endif()
