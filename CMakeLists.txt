cmake_minimum_required(VERSION 3.24)

# Automatically read plugin metadata from plugin.json so all branches share this single CMakeLists.txt!
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" PLUGIN_MANIFEST_JSON)
string(JSON PLUGIN_ID GET "${PLUGIN_MANIFEST_JSON}" "id")
string(JSON PLUGIN_LIB GET "${PLUGIN_MANIFEST_JSON}" "library")
string(JSON PLUGIN_VER GET "${PLUGIN_MANIFEST_JSON}" "version")
string(REGEX REPLACE "\\.dll$" "" PLUGIN_SLUG "${PLUGIN_LIB}")

project("MemeConvertStudio-${PLUGIN_SLUG}" VERSION "${PLUGIN_VER}" LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "MemeConvertStudio plugins target Windows 10 (1903+) or newer.")
endif()

option(MEMEPLUGIN_BUILD_TESTS "Build standalone plugin contract and unit tests" ON)

add_library(plugin_compiler_flags INTERFACE)
target_compile_features(plugin_compiler_flags INTERFACE cxx_std_20)
target_compile_definitions(plugin_compiler_flags INTERFACE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(plugin_compiler_flags INTERFACE /utf-8 /W4 /EHsc /permissive-)
endif()

file(GLOB PLUGIN_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(${PLUGIN_SLUG} SHARED ${PLUGIN_SOURCES})
set_target_properties(${PLUGIN_SLUG} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${PLUGIN_SLUG}")
target_include_directories(${PLUGIN_SLUG} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
target_include_directories(${PLUGIN_SLUG} SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_link_libraries(${PLUGIN_SLUG}
    PRIVATE plugin_compiler_flags user32 gdi32 msimg32 comctl32 comdlg32 shell32 ole32 uuid crypt32 bcrypt advapi32)
if(MSVC)
    set_property(TARGET ${PLUGIN_SLUG} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Stage clean plugin bundle into out/dist/<config>/plugins/<slug>/ (zero .obj/.lib/.exp pollution!)
set(PLUGIN_STAGE_DIR "${CMAKE_BINARY_DIR}/dist/$<CONFIG>/plugins/${PLUGIN_SLUG}")
add_custom_command(TARGET ${PLUGIN_SLUG} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGIN_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PLUGIN_SLUG}>" "${PLUGIN_STAGE_DIR}/${PLUGIN_LIB}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "${PLUGIN_STAGE_DIR}/plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/README.md" "${PLUGIN_STAGE_DIR}/README.md"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources")
    add_custom_command(TARGET ${PLUGIN_SLUG} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources" "${PLUGIN_STAGE_DIR}/resources")
endif()

if(MEMEPLUGIN_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/plugin_test.cpp")
    enable_testing()
    file(GLOB EXTRA_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
    list(REMOVE_ITEM EXTRA_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/mock_waifu.cpp")

    add_executable(PluginTests ${EXTRA_TEST_SOURCES})
    target_include_directories(PluginTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
    target_include_directories(PluginTests SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
    target_link_libraries(PluginTests PRIVATE plugin_compiler_flags ole32 uuid crypt32 bcrypt advapi32)
    if(MSVC)
        set_property(TARGET PluginTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_link_options(PluginTests PRIVATE /ENTRY:wmainCRTStartup)
    endif()
    add_dependencies(PluginTests ${PLUGIN_SLUG})

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/mock_waifu.cpp")
        add_executable(mock-waifu "${CMAKE_CURRENT_SOURCE_DIR}/tests/mock_waifu.cpp")
        target_include_directories(mock-waifu PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
        target_link_libraries(mock-waifu PRIVATE plugin_compiler_flags windowscodecs ole32 uuid crypt32 bcrypt)
        if(MSVC)
            set_property(TARGET mock-waifu PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
            target_link_options(mock-waifu PRIVATE /ENTRY:wmainCRTStartup)
        endif()
        add_dependencies(PluginTests mock-waifu)
        add_test(NAME "${PLUGIN_SLUG}-tests"
            COMMAND PluginTests "${PLUGIN_STAGE_DIR}" "$<TARGET_FILE:mock-waifu>")
    else()
        add_test(NAME "${PLUGIN_SLUG}-tests"
            COMMAND PluginTests "${PLUGIN_STAGE_DIR}")
    endif()
endif()
