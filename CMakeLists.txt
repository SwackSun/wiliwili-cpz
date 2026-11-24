cmake_minimum_required(VERSION 3.21)
project(wiliwili_cm0 LANGUAGES C CXX)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(WILIWILI_INTERNAL_VERSION "v1.6.1.rc1")
set(WILIWILI_MEDIA_OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/build/media" CACHE PATH "Buildroot media output")
set(WILIWILI_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "Root of the wiliwili source checkout")

set(upstream_checkout "${CMAKE_CURRENT_SOURCE_DIR}/upstream")
set(sdl "${upstream_checkout}/library/borealis/library/lib/extern/SDL")
if(NOT EXISTS "${sdl}/include/SDL.h")
    message(FATAL_ERROR "Prepare the pinned wiliwili upstream checkout. See README.md.")
endif()

# Build the actual application and its pinned SDL with an EGL pbuffer.
set(PLATFORM_DESKTOP ON CACHE BOOL "")
set(USE_SDL2 ON CACHE BOOL "")
set(USE_GLES2 ON CACHE BOOL "")
# Keep playback independent of GLES3 shader support on VideoCore IV.
set(MPV_SW_RENDER ON CACHE BOOL "")
set(SDL_X11 OFF CACHE BOOL "")
set(SDL_WAYLAND OFF CACHE BOOL "")
set(SDL_KMSDRM OFF CACHE BOOL "")
set(SDL_OFFSCREEN ON CACHE BOOL "")
set(SDL_SHARED OFF CACHE BOOL "")
set(SDL_STATIC ON CACHE BOOL "")
set(USE_SYSTEM_CURL ON CACHE BOOL "")
set(SIMPLE_HIGHLIGHT ON CACHE BOOL "")
set(CUSTOM_RESOURCES_DIR "/usr/share/wiliwili" CACHE STRING "")
add_compile_definitions(NO_GA)
# Some Borealis includes use the installed SDL2/ prefix even for a bundled build.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include")
file(CREATE_LINK "${sdl}/include" "${CMAKE_CURRENT_BINARY_DIR}/include/SDL2" SYMBOLIC)
include_directories("${CMAKE_CURRENT_BINARY_DIR}/include")
function(apply_client_patch source patch_file board_source)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}" "${patch_file}")
    execute_process(COMMAND patch --batch --fuzz=0 ${ARGN} -o "${board_source}.tmp" "${source}" -i "${patch_file}"
        COMMAND_ERROR_IS_FATAL ANY)
    file(COPY_FILE "${board_source}.tmp" "${board_source}" ONLY_IF_DIFFERENT)
endfunction()

# Configure the board build without network libraries from the upstream DLNA stack.
set(upstream "${CMAKE_CURRENT_BINARY_DIR}/port-upstream")
file(MAKE_DIRECTORY "${upstream}/library")
foreach(dir cmake scripts wiliwili)
    file(CREATE_LINK "${upstream_checkout}/${dir}" "${upstream}/${dir}" SYMBOLIC)
endforeach()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(art "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set(port_resources "${CMAKE_CURRENT_BINARY_DIR}/cardputerzero-resources")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${art}/generate.py" "${art}/icons.json" "${art}/LUCIDE-LICENSE"
    "${art}/FEATHER-LICENSE" "${art}/NOTO-EMOJI-OFL" "${art}/NOTICE.md")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${art}/generate.py"
    "${upstream_checkout}/resources" "${port_resources}" COMMAND_ERROR_IS_FATAL ANY)
file(CREATE_LINK "${port_resources}" "${upstream}/resources" SYMBOLIC)
foreach(dir borealis cpr OpenCC QR-Code-generator pystring lunasvg MemoryModule)
    file(CREATE_LINK "${upstream_checkout}/library/${dir}" "${upstream}/library/${dir}" SYMBOLIC)
endforeach()
apply_client_patch("${upstream_checkout}/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/upstream-build.patch" "${upstream}/CMakeLists.txt")
apply_client_patch("${upstream_checkout}/library/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/upstream-libraries.patch" "${upstream}/library/CMakeLists.txt")
add_subdirectory("${upstream}" upstream EXCLUDE_FROM_ALL)

# Upstream git_info() uses CMAKE_SOURCE_DIR, which here belongs to the firmware repository.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/UPSTREAM_REVISION")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/UPSTREAM_REVISION" client_revision)
    string(SUBSTRING "${client_revision}" 0 12 client_commit)
else()
    execute_process(COMMAND git -C "${upstream_checkout}" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE client_commit OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
endif()
set_property(SOURCE "${upstream}/wiliwili/source/utils/version_helper.cpp" DIRECTORY "${upstream}"
    APPEND PROPERTY COMPILE_OPTIONS
    "-UBUILD_TAG_VERSION" "-DBUILD_TAG_VERSION=${WILIWILI_INTERNAL_VERSION}"
    "-UBUILD_TAG_SHORT" "-DBUILD_TAG_SHORT=${client_commit}")

get_target_property(client_sources wiliwili SOURCES)
list(REMOVE_ITEM client_sources "${upstream}/wiliwili/source/main.cpp")
function(patch_client_source relative_source patch_name)
    set(source "${upstream}/wiliwili/source/${relative_source}")
    set(patch_file "${CMAKE_CURRENT_SOURCE_DIR}/patches/${patch_name}.patch")
    set(board_source "${CMAKE_CURRENT_BINARY_DIR}/${patch_name}.cpp")
    apply_client_patch("${source}" "${patch_file}" "${board_source}" ${ARGN})
    list(REMOVE_ITEM client_sources "${source}")
    set(client_sources ${client_sources} "${board_source}" PARENT_SCOPE)
endfunction()
patch_client_source(utils/config_helper.cpp playback-quality)
patch_client_source(view/mpv_core.cpp software-render)
patch_client_source(view/video_snapshot_core.cpp seek-preview)
configure_file(src/video-quality.hpp video-quality.hpp COPYONLY)
patch_client_source(presenter/video_detail.cpp video-quality)
patch_client_source(activity/player_base_activity.cpp select-quality)
patch_client_source(api/bilibili.cpp api-init)
patch_client_source(api/util/http.cpp cookie-header)
patch_client_source(api/video_detail_api.cpp video-api)
patch_client_source(api/home_api.cpp home-api)
patch_client_source(api/util/wbi.cpp wbi-api)
patch_client_source(presenter/live_data.cpp live-api)
patch_client_source(fragment/mine_qr_login.cpp qr-ui)
patch_client_source(api/mine_api.cpp passport-api)
patch_client_source(fragment/mine_tab.cpp logout-ui)
patch_client_source(activity/setting_activity.cpp tls-settings)
patch_client_source(utils/activity_helper.cpp no-dlna-activity)
patch_client_source(view/video_view.cpp no-dlna-player)
patch_client_source(utils/string_helper.cpp openssl-base64)
patch_client_source(api/util/ws_utils.cpp danmaku-packet)
# The pinned source has whitespace-only lines in the replaced function.
patch_client_source(activity/live_player_activity.cpp live-player-lifetime --ignore-whitespace)

# Upstream includes the same HTTP header both with and without the api/ prefix.
set(account_include "${CMAKE_CURRENT_BINARY_DIR}/account-include")
file(MAKE_DIRECTORY "${account_include}/api/bilibili/util" "${account_include}/api/bilibili/result")
set(image_include "${CMAKE_CURRENT_BINARY_DIR}/image-include")
file(MAKE_DIRECTORY "${image_include}/utils")
apply_client_patch("${upstream}/wiliwili/include/utils/image_helper.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/handheld-images.patch" "${image_include}/utils/image_helper.hpp")
apply_client_patch("${upstream}/wiliwili/include/api/bilibili/result/live_danmaku_result.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/danmaku-host.patch" "${account_include}/api/bilibili/result/live_danmaku_result.h")
apply_client_patch("${upstream}/wiliwili/include/api/bilibili/util/http.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/http-credentials.patch" "${account_include}/api/bilibili/util/http.hpp")
set(player_include "${CMAKE_CURRENT_BINARY_DIR}/player-include")
file(MAKE_DIRECTORY "${player_include}/presenter")
apply_client_patch("${upstream}/wiliwili/include/presenter/video_detail.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/video-request.patch" "${player_include}/presenter/video_detail.hpp")
set(live_include "${CMAKE_CURRENT_BINARY_DIR}/live-include")
configure_file(src/danmaku_live.hpp "${live_include}/live/danmaku_live.hpp" COPYONLY)
target_include_directories(wiliwili BEFORE PRIVATE "${player_include}" "${image_include}" "${account_include}"
    "${account_include}/api" "${live_include}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_property(TARGET wiliwili PROPERTY SOURCES "${client_sources}")
target_sources(wiliwili PRIVATE src/client_main.cpp src/platform.cpp src/device-runtime.cpp
    src/native-video.cpp src/video-cdn.cpp src/linear-surface.cpp)
target_include_directories(wiliwili PRIVATE "${sdl}/src/events")
target_sources(wiliwili PRIVATE src/account-store.cpp src/account-http.cpp src/account-sync.cpp src/account-network.cpp)
target_sources(wiliwili PRIVATE src/danmaku-live.cpp src/danmaku-socket.cpp)
find_path(DRM_INCLUDE_DIR drm.h PATH_SUFFIXES libdrm REQUIRED)
target_include_directories(wiliwili PRIVATE "${DRM_INCLUDE_DIR}")
target_link_libraries(wiliwili PRIVATE GLESv2 EGL crypto drm)
foreach(symbol mpv_render_context_render nvgUpdateImage SDL_GL_CreateContext SDL_CreateWindow SDL_PollEvent SDL_GL_SwapWindow
               SDL_SetWindowSize SDL_SetWindowMinimumSize SDL_SetWindowMaximumSize SDL_SetWindowFullscreen)
    target_link_options(wiliwili PRIVATE "LINKER:--wrap=${symbol}")
endforeach()
set_target_properties(wiliwili PROPERTIES INSTALL_RPATH "$ORIGIN/../lib/wiliwili")
target_link_options(wiliwili PRIVATE "LINKER:--disable-new-dtags")
add_custom_target(cm0_client ALL DEPENDS wiliwili)
install(TARGETS wiliwili RUNTIME DESTINATION bin COMPONENT wiliwili)
install(CODE [[
    # Clear copies installed by earlier versions when reusing a package staging tree.
    if(NOT "$ENV{DESTDIR}" STREQUAL "")
        set(legacy_resources "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/wiliwili/resources")
        file(REMOVE "${legacy_resources}/font/keymap_xbox.ttf" "${legacy_resources}/font/keymap_ps.ttf"
            "${legacy_resources}/font/FREEWARE.txt" "${legacy_resources}/icon/icon.jpg"
            "${legacy_resources}/icon/logo.gif")
        foreach(theme light dark)
            foreach(strength RANGE 3)
                file(REMOVE "${legacy_resources}/img/sys/backup/wifi_${strength}_${theme}.png")
            endforeach()
        endforeach()
    endif()
]] COMPONENT wiliwili)
install(DIRECTORY "${port_resources}/" DESTINATION share/wiliwili/resources COMPONENT wiliwili)
install(FILES upstream/LICENSE DESTINATION share/doc/wiliwili COMPONENT wiliwili)
install(FILES "${art}/GCC-RUNTIME" DESTINATION share/doc/wiliwili/licenses/toolchain COMPONENT wiliwili)
install(FILES USAGE.md DESTINATION share/doc/wiliwili RENAME README.md COMPONENT wiliwili)
install(DIRECTORY patches/media/mpv DESTINATION share/doc/wiliwili/patches COMPONENT wiliwili)
foreach(dep OpenCC borealis cpr pystring lunasvg borealis/library/lib/extern/SDL
        borealis/library/lib/extern/fmt borealis/library/lib/extern/nanovg
        borealis/library/lib/extern/tweeny borealis/library/lib/extern/yoga)
    file(GLOB notices "${upstream}/library/${dep}/LICENSE" "${upstream}/library/${dep}/LICENSE.txt"
                      "${upstream}/library/${dep}/NOTICE")
    install(FILES ${notices} DESTINATION "share/doc/wiliwili/licenses/${dep}" COMPONENT wiliwili)
endforeach()
install(FILES upstream/library/OpenCC/deps/marisa-0.2.6/COPYING.md
        DESTINATION share/doc/wiliwili/licenses/marisa COMPONENT wiliwili)
install(FILES upstream/library/QR-Code-generator/cpp/qrcodegen.hpp
        DESTINATION share/doc/wiliwili/licenses/QR-Code-generator COMPONENT wiliwili)
set(media_notices "${WILIWILI_MEDIA_OUTPUT}/legal-info")
install(DIRECTORY "${media_notices}/licenses/" DESTINATION share/doc/wiliwili/licenses/media COMPONENT wiliwili)
install(FILES "${media_notices}/manifest.csv" DESTINATION share/doc/wiliwili/licenses/media COMPONENT wiliwili)
configure_file(cmake/install-runtime.cmake.in install-runtime.cmake @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/install-runtime.cmake" COMPONENT wiliwili)
