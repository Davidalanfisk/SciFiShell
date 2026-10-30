cmake_minimum_required(VERSION 3.20)
project(SciFiShell LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# ---- Diligent Engine (core + engine backends) -------------------------------------------------
# Override with -DSCIFI_DILIGENT_TAG=<tag> to pin a specific release.
set(SCIFI_DILIGENT_TAG "master" CACHE STRING "DiligentCore git tag/branch")
set(DILIGENT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DILIGENT_NO_FORMAT_VALIDATION ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_OPENGL ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_DIRECT3D11 ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_WEBGPU ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_ARCHIVER ON CACHE BOOL "" FORCE)
if(WIN32)
    set(DILIGENT_NO_VULKAN ON CACHE BOOL "" FORCE)
elseif(APPLE)
    set(DILIGENT_NO_VULKAN ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(DiligentCore
    GIT_REPOSITORY https://github.com/DiligentGraphics/DiligentCore.git
    GIT_TAG        ${SCIFI_DILIGENT_TAG}
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(DiligentCore)

# ---- OpenXR loader ----------------------------------------------------------------------------
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(OpenXR
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.0.34
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(OpenXR)

# ---- SDL2 -------------------------------------------------------------------------------------
find_package(SDL2 REQUIRED CONFIG)

# ---- Application ------------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/AppShell.cpp
    src/TaskSwitcher.cpp
    src/Renderer.cpp
    src/SceneGraph.cpp
    src/LCARSUI.cpp
    src/Theme.cpp
    src/Settings.cpp
    src/SettingsPanel.cpp
    src/SettingsHub.cpp
    src/UiKit.cpp
    src/PanelApp.cpp
    src/SysMonitor.cpp
    src/Terminal.cpp
    src/FileBrowser.cpp
    src/TextEditor.cpp
    src/ToolRing.cpp
    src/FileRoom.cpp
    src/InputSystem.cpp
    src/VRSystem.cpp
    src/XInputController.cpp
    src/Utils.cpp
    src/City.cpp
    src/CityData.cpp
    src/TextRender.cpp
    src/Thumbnails.cpp
    src/MediaPlayer.cpp
    src/MediaLibrary.cpp
    src/MediaMeta.cpp
    src/MediaApp.cpp
    src/FolderPicker.cpp
    src/AppShellMedia.cpp
    src/CodeEditor.cpp
    src/Viewer.cpp
    src/VideoFrame.cpp
    src/ModelLoader.cpp
    src/StbImpl.cpp)
if(WIN32)
    list(APPEND SOURCES src/AppWindow.cpp src/AppCatalog.cpp src/OsSettings.cpp)
endif()

# ---- Chromium Embedded Framework (Windows only for now) ----------------------------------------
set(SCIFI_HAS_CEF OFF)
set(SCIFI_CEF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cef" CACHE PATH "CEF binary distribution")
if(WIN32 AND EXISTS "${SCIFI_CEF_ROOT}/libcef_dll/CMakeLists.txt")
    set(SCIFI_HAS_CEF ON)
    set(CEF_ROOT "${SCIFI_CEF_ROOT}")
    list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
    set(USE_SANDBOX OFF CACHE BOOL "" FORCE)
    set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "" FORCE)
    find_package(CEF REQUIRED)
    add_subdirectory("${CEF_ROOT}/libcef_dll" libcef_dll_wrapper)
    list(APPEND SOURCES src/WebBrowser.cpp src/WebLayerAnalyzer.cpp src/WebDepthCompositor.cpp)
endif()

if(WIN32)
    list(APPEND SOURCES assets/icon/app.rc)
endif()
add_executable(SciFiShell ${SOURCES})
if(WIN32)
    # Out-of-process UI Automation clicker for hosted Windows programs (see src/UiaHelper.cpp).
    add_executable(SciFiUia src/UiaHelper.cpp)
    target_link_libraries(SciFiUia PRIVATE ole32 oleaut32 uuid)
    add_dependencies(SciFiShell SciFiUia)
endif()
target_include_directories(SciFiShell PRIVATE src third_party)
target_compile_definitions(SciFiShell PRIVATE SDL_MAIN_HANDLED NOMINMAX)
if(MSVC)
    target_compile_options(SciFiShell PRIVATE /Zc:char8_t-)  # keep u8path/u8string as std::string (CEF needs C++20)
endif()

if(TARGET SDL2::SDL2)
    target_link_libraries(SciFiShell PRIVATE SDL2::SDL2)
else()
    target_include_directories(SciFiShell PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(SciFiShell PRIVATE ${SDL2_LIBRARIES})
endif()

target_link_libraries(SciFiShell PRIVATE
    OpenXR::openxr_loader
    Diligent-Common
    Diligent-GraphicsTools
    Diligent-BuildSettings)

if(WIN32)
    target_link_libraries(SciFiShell PRIVATE Diligent-GraphicsEngineD3D12-static xinput d3d12 dxgi pdh
                          mfplat mfreadwrite mfuuid propsys ole32 oleaut32 iphlpapi powrprof ws2_32 psapi)
elseif(APPLE)
    target_link_libraries(SciFiShell PRIVATE Diligent-GraphicsEngineMetal-static)
else()
    target_link_libraries(SciFiShell PRIVATE Diligent-GraphicsEngineVk-static)
endif()

if(SCIFI_HAS_CEF)
    target_compile_definitions(SciFiShell PRIVATE SCIFI_HAS_CEF=1)
    target_include_directories(SciFiShell PRIVATE "${CEF_ROOT}")
    target_link_libraries(SciFiShell PRIVATE libcef_dll_wrapper "${CEF_ROOT}/Release/libcef.lib")
    # CEF runtime files must sit next to the exe (it doubles as the CEF subprocess).
    add_custom_command(TARGET SciFiShell POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CEF_ROOT}/Release" $<TARGET_FILE_DIR:SciFiShell>
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CEF_ROOT}/Resources" $<TARGET_FILE_DIR:SciFiShell>)
endif()

# Shaders are loaded at runtime from <exe dir>/shaders.
add_custom_command(TARGET SciFiShell POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:SciFiShell>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:SciFiShell>/assets)
