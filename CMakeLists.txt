cmake_minimum_required(VERSION 3.20)
project(SciFiShell LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# Restricted script extensions and ZIP package reader.
FetchContent_Declare(scifi_lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG 6e22fedb74cf0c9b6656e9fce8b7331db847c605)
FetchContent_MakeAvailable(scifi_lua)
set(SCIFI_LUA_CORE lapi lauxlib lcode lctype ldebug ldo ldump lfunc lgc llex lmathlib lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm lzio)
set(SCIFI_LUA_SOURCES)
foreach(source IN LISTS SCIFI_LUA_CORE)
    list(APPEND SCIFI_LUA_SOURCES "${scifi_lua_SOURCE_DIR}/${source}.c")
endforeach()
add_library(scifi_lua STATIC ${SCIFI_LUA_SOURCES})
target_include_directories(scifi_lua PUBLIC "${scifi_lua_SOURCE_DIR}")

FetchContent_Declare(scifi_miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 77d0dce8627735138c51770d1799a1ef48f2117d)
FetchContent_MakeAvailable(scifi_miniz)

FetchContent_Declare(scifi_stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 4e9b368bfc071e97206ea5c6adba68dd0cbf8897)
FetchContent_MakeAvailable(scifi_stb)
add_library(scifi_gameplay_audio STATIC src/GameplayAudio.cpp)
target_include_directories(scifi_gameplay_audio PRIVATE "${scifi_stb_SOURCE_DIR}")

FetchContent_Declare(scifi_picosha2
    GIT_REPOSITORY https://github.com/okdshin/PicoSHA2.git
    GIT_TAG 161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29)
FetchContent_MakeAvailable(scifi_picosha2)
add_library(scifi_gameplay_digest STATIC src/GameplayDigest.cpp)
target_include_directories(scifi_gameplay_digest PRIVATE "${scifi_picosha2_SOURCE_DIR}")

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

# ---- SQLite (media library database) ----------------------------------------------------------
FetchContent_Declare(sqlite3src
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip)
FetchContent_GetProperties(sqlite3src)
if(NOT sqlite3src_POPULATED)
    FetchContent_Populate(sqlite3src)
endif()
add_library(sqlite3_static STATIC ${sqlite3src_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3_static PUBLIC ${sqlite3src_SOURCE_DIR})
target_compile_definitions(sqlite3_static PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)

# ---- SDL2 -------------------------------------------------------------------------------------
find_package(SDL2 REQUIRED CONFIG)

# ---- Application ------------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/AppShell.cpp
    src/AppShellGameplay.cpp
    src/AppShellDowntown.cpp
    src/AppShellSettingsDistrict.cpp
    src/Downtown.cpp
    src/plugins/AppShellCar.cpp
    src/plugins/AppShellParks.cpp
    src/TaskSwitcher.cpp
    src/Harvi.cpp
    src/Renderer.cpp
    src/SceneGraph.cpp
    src/LCARSUI.cpp
    src/Theme.cpp
    src/Settings.cpp
    src/PluginManager.cpp
    src/PluginPackage.cpp
    src/PluginScript.cpp
    src/GameplayArchive.cpp
    src/GameplayTexture.cpp
    src/GameplayModel.cpp
    src/GameplayGpuValidation.cpp
    src/GameplaySceneRenderer.cpp
    src/GameplayPackage.cpp
    src/GameplayConsent.cpp
    src/GameplaySession.cpp
    src/GameplayStorage.cpp
    src/NativePlugin.cpp
    src/SettingsPanel.cpp
    src/SettingsHub.cpp
    src/UiKit.cpp
    src/PanelApp.cpp
    src/SysMonitor.cpp
    src/OnScreenKeyboard.cpp
    src/Terminal.cpp
    src/FileBrowser.cpp
    src/TextEditor.cpp
    src/ImageEditor.cpp
    src/FtpApp.cpp
    src/Calculator.cpp
    src/StartMenu.cpp
    src/FileRoom.cpp
    src/InputSystem.cpp
    src/VRSystem.cpp
    src/XInputController.cpp
    src/Utils.cpp
    src/Math.cpp
    src/City.cpp
    src/plugins/Parks.cpp
    src/plugins/CarRender.cpp
    src/plugins/Track.cpp
    src/Sfx.cpp
    src/CityData.cpp
    src/TextRender.cpp
    src/Thumbnails.cpp
    src/MediaPlayer.cpp
    src/MediaLibrary.cpp
    src/MediaMeta.cpp
    src/Net.cpp
    src/MediaDb.cpp
    src/MediaScrape.cpp
    src/MediaApp.cpp
    src/AppShellMedia.cpp
    src/AppShellWindows.cpp
    src/CodeEditor.cpp
    src/Viewer.cpp
    src/VideoFrame.cpp
    src/ModelLoader.cpp
    src/StbImpl.cpp)
if(WIN32)
    list(APPEND SOURCES src/AppWindow.cpp src/WinCapture.cpp src/AppCatalog.cpp src/OsSettings.cpp)
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
option(SCIFI_TEST_SYNTHETIC_CITY "Build an isolated synthetic-city integration test executable, not for normal use" OFF)
if(SCIFI_TEST_SYNTHETIC_CITY)
    target_compile_definitions(SciFiShell PRIVATE SCIFI_TEST_SYNTHETIC_CITY=1)
endif()
add_library(TrackSamplingModule MODULE src/plugins/TrackSamplingModule.cpp src/plugins/TrackGenerationModule.cpp src/plugins/TrackRenderingModule.cpp src/plugins/DrivingModule.cpp src/plugins/CarRenderingModule.cpp src/Math.cpp)
add_library(ParksSceneModule MODULE src/plugins/ParksSceneModule.cpp src/plugins/ParksSceneRendering.cpp src/plugins/ParksToysModule.cpp src/Math.cpp)
target_include_directories(TrackSamplingModule PRIVATE src)
target_include_directories(ParksSceneModule PRIVATE src)
add_dependencies(SciFiShell TrackSamplingModule ParksSceneModule)
target_compile_definitions(SciFiShell PRIVATE SCIFI_TRACK_MODULE_FILENAME="$<TARGET_FILE_NAME:TrackSamplingModule>"
    SCIFI_PARKS_MODULE_FILENAME="$<TARGET_FILE_NAME:ParksSceneModule>")
target_link_libraries(SciFiShell PRIVATE ${CMAKE_DL_LIBS})

include(CTest)
if(BUILD_TESTING)
    add_executable(SettingsModelTest tests/SettingsModelTest.cpp)
    target_include_directories(SettingsModelTest PRIVATE src)
    target_compile_definitions(SettingsModelTest PRIVATE NOMINMAX)
    add_test(NAME SettingsModelTest COMMAND SettingsModelTest)
    add_executable(DowntownTest tests/DowntownTest.cpp src/Downtown.cpp)
    target_include_directories(DowntownTest PRIVATE src)
    target_compile_definitions(DowntownTest PRIVATE NOMINMAX)
    add_test(NAME DowntownTest COMMAND DowntownTest)
    add_executable(GameplayAudioTest tests/GameplayAudioTest.cpp)
    target_include_directories(GameplayAudioTest PRIVATE src)
    target_compile_definitions(GameplayAudioTest PRIVATE "SCIFI_TEST_ASSETS=\"${CMAKE_CURRENT_SOURCE_DIR}/assets\"")
    target_link_libraries(GameplayAudioTest PRIVATE scifi_gameplay_audio)
    add_test(NAME GameplayAudioTest COMMAND GameplayAudioTest)
    add_executable(GameplaySoundTest tests/GameplaySoundTest.cpp src/Sfx.cpp)
    target_include_directories(GameplaySoundTest PRIVATE src)
    target_compile_definitions(GameplaySoundTest PRIVATE SDL_MAIN_HANDLED)
    if(TARGET SDL2::SDL2)
        target_link_libraries(GameplaySoundTest PRIVATE SDL2::SDL2)
    else()
        target_include_directories(GameplaySoundTest PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(GameplaySoundTest PRIVATE ${SDL2_LIBRARIES})
    endif()
    add_test(NAME GameplaySoundTest COMMAND GameplaySoundTest)
    add_executable(GameplayConsentLayoutTest tests/GameplayConsentLayoutTest.cpp src/TextRender.cpp src/StbImpl.cpp)
    target_include_directories(GameplayConsentLayoutTest PRIVATE src third_party)
    target_compile_definitions(GameplayConsentLayoutTest PRIVATE "SCIFI_TEST_FONT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts\"")
    add_test(NAME GameplayConsentLayoutTest COMMAND GameplayConsentLayoutTest)
    add_executable(PluginManagerTest tests/PluginManagerTest.cpp src/PluginManager.cpp)
    target_include_directories(PluginManagerTest PRIVATE src)
    target_compile_definitions(PluginManagerTest PRIVATE NOMINMAX)
    add_test(NAME PluginManagerTest COMMAND PluginManagerTest)
    add_executable(PluginPackageTest tests/PluginPackageTest.cpp tests/CgltfTestImpl.cpp src/PluginPackage.cpp src/PluginManager.cpp src/GameplayArchive.cpp src/GameplayTexture.cpp src/GameplayModel.cpp src/GameplayGpuValidation.cpp src/GameplayPackage.cpp src/GameplayConsent.cpp src/GameplaySession.cpp src/GameplayStorage.cpp src/PluginScript.cpp)
    target_include_directories(PluginPackageTest PRIVATE src third_party)
    target_compile_definitions(PluginPackageTest PRIVATE "SCIFI_TEST_ASSETS=\"${CMAKE_CURRENT_SOURCE_DIR}/assets\"")
    target_link_libraries(PluginPackageTest PRIVATE miniz scifi_lua scifi_gameplay_audio scifi_gameplay_digest)
    add_test(NAME PluginPackageTest COMMAND PluginPackageTest)
    # Explicit, resource-intensive boundary test; not part of headless CTest.
    add_executable(GameplayArchiveLargeTest EXCLUDE_FROM_ALL tests/GameplayArchiveLargeTest.cpp src/GameplayArchive.cpp)
    target_include_directories(GameplayArchiveLargeTest PRIVATE src)
    target_link_libraries(GameplayArchiveLargeTest PRIVATE miniz)
    # Explicitly run on a graphics-capable isolated display; not part of headless CTest.
    add_executable(GameplayRendererTest EXCLUDE_FROM_ALL tests/GameplayRendererTest.cpp src/Renderer.cpp
        src/GameplayGpuValidation.cpp src/GameplayModel.cpp src/GameplayTexture.cpp
        src/GameplaySceneRenderer.cpp src/GameplaySession.cpp src/GameplayPackage.cpp
        src/GameplayArchive.cpp src/GameplayConsent.cpp src/GameplayStorage.cpp src/PluginPackage.cpp src/PluginScript.cpp
        src/ModelLoader.cpp src/StbImpl.cpp src/Math.cpp)
    target_include_directories(GameplayRendererTest PRIVATE src third_party)
    target_compile_definitions(GameplayRendererTest PRIVATE SDL_MAIN_HANDLED NOMINMAX
        "SCIFI_TEST_SHADER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders\""
        "SCIFI_TEST_ASSETS=\"${CMAKE_CURRENT_SOURCE_DIR}/assets\""
        "SCIFI_TEST_WORK_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
    if(TARGET SDL2::SDL2)
        target_link_libraries(GameplayRendererTest PRIVATE SDL2::SDL2)
    else()
        target_include_directories(GameplayRendererTest PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(GameplayRendererTest PRIVATE ${SDL2_LIBRARIES})
    endif()
    target_link_libraries(GameplayRendererTest PRIVATE miniz scifi_lua scifi_gameplay_audio scifi_gameplay_digest Diligent-Common Diligent-GraphicsTools Diligent-BuildSettings)
    if(WIN32)
        target_link_libraries(GameplayRendererTest PRIVATE Diligent-GraphicsEngineD3D12-static d3d12 dxgi)
    elseif(APPLE)
        target_link_libraries(GameplayRendererTest PRIVATE Diligent-GraphicsEngineMetal-static)
    else()
        target_link_libraries(GameplayRendererTest PRIVATE Diligent-GraphicsEngineVk-static)
    endif()
    add_executable(PluginScriptTest tests/PluginScriptTest.cpp src/PluginScript.cpp src/GameplayStorage.cpp)
    target_include_directories(PluginScriptTest PRIVATE src)
    target_link_libraries(PluginScriptTest PRIVATE scifi_lua)
    add_test(NAME PluginScriptTest COMMAND PluginScriptTest)
    add_executable(GameplayStorageTest tests/GameplayStorageTest.cpp src/PluginScript.cpp src/GameplayStorage.cpp)
    target_include_directories(GameplayStorageTest PRIVATE src)
    target_link_libraries(GameplayStorageTest PRIVATE scifi_lua)
    add_test(NAME GameplayStorageTest COMMAND GameplayStorageTest)
    add_executable(GameplayControllerInputTest tests/GameplayControllerInputTest.cpp)
    target_include_directories(GameplayControllerInputTest PRIVATE src)
    add_test(NAME GameplayControllerInputTest COMMAND GameplayControllerInputTest)
    add_library(NativePluginFixture MODULE tests/NativePluginFixture.cpp)
    target_include_directories(NativePluginFixture PRIVATE src)
    foreach(mode IN ITEMS BAD_ABI FAIL_CREATE FAIL_START)
        add_library(NativePluginFixture_${mode} MODULE tests/NativePluginFixture.cpp)
        target_include_directories(NativePluginFixture_${mode} PRIVATE src)
        target_compile_definitions(NativePluginFixture_${mode} PRIVATE FIXTURE_${mode})
    endforeach()
    add_executable(NativePluginTest tests/NativePluginTest.cpp src/NativePlugin.cpp)
    target_include_directories(NativePluginTest PRIVATE src)
    target_link_libraries(NativePluginTest PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(NativePluginTest NativePluginFixture NativePluginFixture_BAD_ABI
        NativePluginFixture_FAIL_CREATE NativePluginFixture_FAIL_START)
    add_test(NAME NativePluginTest COMMAND NativePluginTest
        $<TARGET_FILE:NativePluginFixture> $<TARGET_FILE:NativePluginFixture_BAD_ABI>
        $<TARGET_FILE:NativePluginFixture_FAIL_CREATE> $<TARGET_FILE:NativePluginFixture_FAIL_START>)
    add_executable(TrackSamplingTest tests/TrackSamplingTest.cpp src/NativePlugin.cpp)
    target_include_directories(TrackSamplingTest PRIVATE src)
    target_link_libraries(TrackSamplingTest PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(TrackSamplingTest TrackSamplingModule ParksSceneModule)
    add_test(NAME TrackSamplingTest COMMAND TrackSamplingTest $<TARGET_FILE:TrackSamplingModule> $<TARGET_FILE:ParksSceneModule>)
    add_executable(ParksSceneTest tests/ParksSceneTest.cpp src/NativePlugin.cpp)
    target_include_directories(ParksSceneTest PRIVATE src)
    target_link_libraries(ParksSceneTest PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(ParksSceneTest ParksSceneModule)
    add_test(NAME ParksSceneTest COMMAND ParksSceneTest $<TARGET_FILE:ParksSceneModule>)
    add_executable(PluginThemeTest tests/PluginThemeTest.cpp src/Theme.cpp)
    target_include_directories(PluginThemeTest PRIVATE src)
    add_test(NAME PluginThemeTest COMMAND PluginThemeTest)
endif()
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
    get_target_property(SCIFI_SDL2_TYPE SDL2::SDL2 TYPE)
    if(WIN32 AND SCIFI_SDL2_TYPE STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET SciFiShell POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:SciFiShell>)
        if(BUILD_TESTING)
            add_custom_command(TARGET GameplaySoundTest POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:GameplaySoundTest>)
        endif()
    endif()
else()
    target_include_directories(SciFiShell PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(SciFiShell PRIVATE ${SDL2_LIBRARIES})
endif()

target_link_libraries(SciFiShell PRIVATE
    scifi_lua miniz scifi_gameplay_audio scifi_gameplay_digest
    sqlite3_static
    OpenXR::openxr_loader
    Diligent-Common
    Diligent-GraphicsTools
    Diligent-BuildSettings)

if(WIN32)
    target_link_libraries(SciFiShell PRIVATE Diligent-GraphicsEngineD3D12-static xinput d3d12 dxgi pdh
                          mfplat mfreadwrite mfuuid d3d11 dwmapi windowsapp propsys ole32 oleaut32 iphlpapi powrprof ws2_32 psapi wininet)
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
