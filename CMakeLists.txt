cmake_minimum_required(VERSION 3.21)
project(wiliwili_cm0 LANGUAGES C CXX)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(WILIWILI_INTERNAL_VERSION "v1.6.2.rc18")
option(WILIWILI_UPSTREAM_ART "Use the original artwork from the upstream checkout" OFF)
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
set(CUSTOM_RESOURCES_DIR "/usr/share/wiliwili-cpz" CACHE STRING "")
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
    "${art}/generate.py" "${art}/lcd_layouts.py" "${art}/icons.json" "${art}/LUCIDE-LICENSE"
    "${art}/FEATHER-LICENSE" "${art}/NOTO-EMOJI-OFL" "${art}/NOTICE.md")
set(art_python "${Python3_EXECUTABLE}")
if(WILIWILI_UPSTREAM_ART)
    # The SDK Python has no Pillow. Use the build host's interpreter for artwork.
    find_program(host_art_python NAMES python3 PATHS ENV PATH NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    set(art_python "${host_art_python}")
    set(art_options --upstream-art)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${art}/lcd-search.xml" "${art}/lcd-tv-search.xml" "${art}/lcd-main.xml" "${art}/lcd-home.xml" "${art}/lcd-qr.xml" "${art}/lcd-recommends.xml")
execute_process(COMMAND "${art_python}" -B "${art}/generate.py" ${art_options}
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

# The pinned Borealis dialog sources use CRLF. Normalize the build copy for patching.
function(patch_borealis_view name patch_name)
    get_target_property(borealis_sources borealis SOURCES)
    set(matches ${borealis_sources})
    list(FILTER matches INCLUDE REGEX "/views/${name}[.]cpp$")
    list(GET matches 0 source)
    set(normalized "${CMAKE_CURRENT_BINARY_DIR}/${name}.cpp")
    configure_file("${source}" "${normalized}" @ONLY NEWLINE_STYLE UNIX)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${patch_name}.cpp")
    apply_client_patch("${normalized}" "${CMAKE_CURRENT_SOURCE_DIR}/patches/${patch_name}.patch" "${output}")
    list(REMOVE_ITEM borealis_sources "${source}")
    set_property(TARGET borealis PROPERTY SOURCES "${borealis_sources};${output}")
endfunction()
patch_borealis_view(edit_text_dialog lcd-ime)
patch_borealis_view(dialog lcd-dialog)

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
patch_client_source(view/danmaku_core.cpp rolling-danmaku)
patch_client_source(view/svg_image.cpp icon-resolution)
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
patch_client_source(view/dynamic_article.cpp lcd-article-focus)
patch_client_source(fragment/player_single_comment.cpp lcd-comment-focus)
patch_client_source(fragment/dynamic_tab.cpp dynamic-feed)
patch_client_source(presenter/dynamic_video.cpp dynamic-feed-requests)
patch_client_source(fragment/home_tab.cpp lcd-home-search)
patch_client_source(fragment/search_history.cpp lcd-history)
patch_client_source(fragment/home_live.cpp lcd-live)
patch_client_source(activity/search_activity.cpp lcd-search)
patch_client_source(fragment/mine_qr_login.cpp qr-ui)
patch_client_source(api/mine_api.cpp passport-api)
patch_client_source(fragment/mine_tab.cpp logout-ui)
patch_client_source(activity/setting_activity.cpp tls-settings)
patch_client_source(utils/activity_helper.cpp no-dlna-activity)
patch_client_source(view/recycling_grid.cpp compact-grid)
patch_client_source(fragment/inbox_view.cpp lcd-inbox)
patch_client_source(fragment/mine_history.cpp lcd-history-byline)
patch_client_source(view/auto_tab_frame.cpp lcd-sidebar)
patch_client_source(view/video_view.cpp no-dlna-player)
patch_client_source(utils/string_helper.cpp openssl-base64)
patch_client_source(api/util/ws_utils.cpp danmaku-packet)
# The pinned source has whitespace-only lines in the replaced function.
patch_client_source(activity/live_player_activity.cpp live-player-lifetime --ignore-whitespace)

# Upstream includes the same HTTP header both with and without the api/ prefix.
set(account_include "${CMAKE_CURRENT_BINARY_DIR}/account-include")
file(MAKE_DIRECTORY "${account_include}/api/bilibili/util" "${account_include}/api/bilibili/result")
set(image_include "${CMAKE_CURRENT_BINARY_DIR}/image-include")
file(MAKE_DIRECTORY "${image_include}/utils" "${image_include}/fragment")
apply_client_patch("${upstream}/wiliwili/include/fragment/dynamic_tab.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/dynamic-feed-header.patch" "${image_include}/fragment/dynamic_tab.hpp")
apply_client_patch("${upstream}/wiliwili/include/fragment/mine_qr_login.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/lcd-qr-close.patch" "${image_include}/fragment/mine_qr_login.hpp")
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
target_sources(wiliwili PRIVATE src/client_main.cpp src/lcd-search-input.cpp src/platform.cpp src/device-runtime.cpp
    src/native-video.cpp src/video-cdn.cpp src/linear-surface.cpp src/danmaku-strip.cpp)
target_include_directories(wiliwili PRIVATE "${sdl}/src/events")
target_sources(wiliwili PRIVATE src/account-store.cpp src/account-http.cpp src/account-sync.cpp src/account-network.cpp)
target_sources(wiliwili PRIVATE src/danmaku-live.cpp src/danmaku-socket.cpp)
find_path(DRM_INCLUDE_DIR drm.h PATH_SUFFIXES libdrm REQUIRED)
target_include_directories(wiliwili PRIVATE "${DRM_INCLUDE_DIR}")
find_package(Freetype REQUIRED)
target_link_libraries(wiliwili PRIVATE GLESv2 EGL crypto drm Freetype::Freetype)
foreach(symbol mpv_render_context_render nvgUpdateImage SDL_EGL_CreateOffscreenSurface SDL_GL_CreateContext SDL_CreateWindow SDL_PollEvent SDL_GL_SwapWindow
               SDL_SetWindowSize SDL_SetWindowMinimumSize SDL_SetWindowMaximumSize SDL_SetWindowFullscreen)
    target_link_options(wiliwili PRIVATE "LINKER:--wrap=${symbol}")
endforeach()
set_target_properties(wiliwili PROPERTIES OUTPUT_NAME "wiliwili-cpz"
    INSTALL_RPATH "$ORIGIN/../lib/wiliwili-cpz")
target_link_options(wiliwili PRIVATE "LINKER:--disable-new-dtags")
add_custom_target(cm0_client ALL DEPENDS wiliwili)
install(TARGETS wiliwili RUNTIME DESTINATION bin COMPONENT wiliwili-cpz)
install(CODE [[
    # Clear copies installed by earlier versions when reusing a package staging tree.
    if(NOT "$ENV{DESTDIR}" STREQUAL "")
        set(legacy_resources "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/wiliwili-cpz/resources")
        file(REMOVE "${legacy_resources}/font/keymap_xbox.ttf" "${legacy_resources}/font/keymap_ps.ttf"
            "${legacy_resources}/font/FREEWARE.txt" "${legacy_resources}/icon/icon.jpg"
            "${legacy_resources}/icon/logo.gif")
        foreach(theme light dark)
            foreach(strength RANGE 3)
                file(REMOVE "${legacy_resources}/img/sys/backup/wifi_${strength}_${theme}.png")
            endforeach()
        endforeach()
    endif()
]] COMPONENT wiliwili-cpz)
install(DIRECTORY "${port_resources}/" DESTINATION share/wiliwili-cpz/resources COMPONENT wiliwili-cpz)
install(FILES upstream/LICENSE DESTINATION share/doc/wiliwili-cpz COMPONENT wiliwili-cpz)
install(FILES "${art}/GCC-RUNTIME" DESTINATION share/doc/wiliwili-cpz/licenses/toolchain COMPONENT wiliwili-cpz)
install(FILES USAGE.md DESTINATION share/doc/wiliwili-cpz RENAME README.md COMPONENT wiliwili-cpz)
install(DIRECTORY patches/media/mpv DESTINATION share/doc/wiliwili-cpz/patches COMPONENT wiliwili-cpz)
foreach(dep OpenCC borealis cpr pystring lunasvg borealis/library/lib/extern/SDL
        borealis/library/lib/extern/fmt borealis/library/lib/extern/nanovg
        borealis/library/lib/extern/tweeny borealis/library/lib/extern/yoga)
    file(GLOB notices "${upstream}/library/${dep}/LICENSE" "${upstream}/library/${dep}/LICENSE.txt"
                      "${upstream}/library/${dep}/NOTICE")
    install(FILES ${notices} DESTINATION "share/doc/wiliwili-cpz/licenses/${dep}" COMPONENT wiliwili-cpz)
endforeach()
install(FILES upstream/library/OpenCC/deps/marisa-0.2.6/COPYING.md
        DESTINATION share/doc/wiliwili-cpz/licenses/marisa COMPONENT wiliwili-cpz)
install(FILES upstream/library/QR-Code-generator/cpp/qrcodegen.hpp
        DESTINATION share/doc/wiliwili-cpz/licenses/QR-Code-generator COMPONENT wiliwili-cpz)
set(media_notices "${WILIWILI_MEDIA_OUTPUT}/legal-info")
install(DIRECTORY "${media_notices}/licenses/" DESTINATION share/doc/wiliwili-cpz/licenses/media COMPONENT wiliwili-cpz)
install(FILES "${media_notices}/manifest.csv" DESTINATION share/doc/wiliwili-cpz/licenses/media COMPONENT wiliwili-cpz)
configure_file(cmake/install-runtime.cmake.in install-runtime.cmake @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/install-runtime.cmake" COMPONENT wiliwili-cpz)
