cmake_minimum_required(VERSION 3.21)

project(BitScopeQt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Concurrent)
qt_standard_project_setup()
if(MSVC)
    # All sources, including smoke tests, contain UTF-8 Chinese text.
    add_compile_options(/utf-8)
endif()
# Some managed macOS installations protect CMake's compiler probe source.
# MOC does not need those optional predefines for this project.
set(CMAKE_AUTOMOC_COMPILER_PREDEFINES OFF)

qt_add_executable(BitScopeQt
    src/main.cpp
    src/platformwindow.h
    src/windowtitlebar.h
    src/windowtitlebar.cpp
    src/bitstreamtypes.h
    src/bitreader.h
    src/bitstreamparser.h
    src/bitstreamparser.cpp
    src/hevcaccess.h
    src/mpeg4access.h
    src/avclearning.h
    src/avcmotionprovider.h
    src/avcmotionprovider.cpp
    src/frametablemodel.h
    src/frametablemodel.cpp
    src/previewprovider.h
    src/previewprovider.cpp
    src/hevccuprovider.h
    src/hevccuprovider.cpp
    src/av1blockprovider.h
    src/av1blockprovider.cpp
    src/macroblockview.h
    src/macroblockview.cpp
    src/timelinewidget.h
    src/timelinewidget.cpp
    src/mainwindow.h
    src/mainwindow.cpp
)

if(APPLE)
    enable_language(OBJCXX)
    target_sources(BitScopeQt PRIVATE src/platformwindow_mac.mm)
    target_link_libraries(BitScopeQt PRIVATE "-framework Cocoa")
endif()
if(WIN32)
    target_sources(BitScopeQt PRIVATE src/platformwindow_win.cpp)
    target_link_libraries(BitScopeQt PRIVATE user32)
endif()

set(BITSCOPE_HEVC_ANALYZER "" CACHE FILEPATH
    "Optional HM_180 analyzer executable to bundle for HEVC CU-tree extraction")
set(BITSCOPE_AVC_MOTION_EXECUTABLE "" CACHE FILEPATH "H264 / MPEG4 motion-vector extraction helper")
if(BITSCOPE_AVC_MOTION_EXECUTABLE AND EXISTS "${BITSCOPE_AVC_MOTION_EXECUTABLE}")
    if(APPLE)
        set(_motion_destination "$<TARGET_BUNDLE_CONTENT_DIR:BitScopeQt>/Resources/tools")
        set(_motion_notices "$<TARGET_BUNDLE_CONTENT_DIR:BitScopeQt>/Resources/licenses/ffmpeg-motion")
    else()
        set(_motion_destination "$<TARGET_FILE_DIR:BitScopeQt>/tools")
        set(_motion_notices "$<TARGET_FILE_DIR:BitScopeQt>/licenses/ffmpeg-motion")
    endif()
    add_custom_command(TARGET BitScopeQt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_motion_destination}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BITSCOPE_AVC_MOTION_EXECUTABLE}" "${_motion_destination}/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-motion" "${_motion_notices}"
        VERBATIM)
    get_filename_component(_motion_source_dir "${BITSCOPE_AVC_MOTION_EXECUTABLE}" DIRECTORY)
    if(EXISTS "${_motion_source_dir}/licenses")
        add_custom_command(TARGET BitScopeQt POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${_motion_source_dir}/licenses" "${_motion_notices}"
            VERBATIM)
    endif()
    if(WIN32)
        file(GLOB _motion_dlls "${_motion_source_dir}/*.dll")
        if(_motion_dlls)
            add_custom_command(TARGET BitScopeQt POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_motion_dlls} "${_motion_destination}/"
                VERBATIM)
        endif()
    endif()
endif()
if(BITSCOPE_HEVC_ANALYZER AND EXISTS "${BITSCOPE_HEVC_ANALYZER}")
    if(WIN32)
        set(_bitscope_hevc_helper_name "HM_180.exe")
    else()
        set(_bitscope_hevc_helper_name "HM_180")
    endif()
    if(APPLE)
        set(_bitscope_resource_dir "$<TARGET_BUNDLE_CONTENT_DIR:BitScopeQt>/Resources")
    else()
        set(_bitscope_resource_dir "$<TARGET_FILE_DIR:BitScopeQt>")
    endif()
    add_custom_command(TARGET BitScopeQt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_bitscope_resource_dir}/tools"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_bitscope_resource_dir}/licenses/hevc-cu-analyzer"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BITSCOPE_HEVC_ANALYZER}"
            "${_bitscope_resource_dir}/tools/${_bitscope_hevc_helper_name}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hevc-cu-analyzer"
            "${_bitscope_resource_dir}/licenses/hevc-cu-analyzer"
        VERBATIM)
endif()

set(BITSCOPE_AV1_INSPECT_EXECUTABLE "" CACHE FILEPATH
    "Optional libaom inspect executable (CONFIG_INSPECTION=1) to bundle for AV1 block extraction")
if(BITSCOPE_AV1_INSPECT_EXECUTABLE AND EXISTS "${BITSCOPE_AV1_INSPECT_EXECUTABLE}")
    if(WIN32)
        set(_bitscope_av1_helper_name "av1inspect.exe")
    else()
        set(_bitscope_av1_helper_name "av1inspect")
    endif()
    if(APPLE)
        set(_bitscope_av1_resource_dir "$<TARGET_BUNDLE_CONTENT_DIR:BitScopeQt>/Resources")
    else()
        set(_bitscope_av1_resource_dir "$<TARGET_FILE_DIR:BitScopeQt>")
    endif()
    add_custom_command(TARGET BitScopeQt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_bitscope_av1_resource_dir}/tools"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_bitscope_av1_resource_dir}/licenses/aom-inspection"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BITSCOPE_AV1_INSPECT_EXECUTABLE}"
            "${_bitscope_av1_resource_dir}/tools/${_bitscope_av1_helper_name}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/aom-inspection"
            "${_bitscope_av1_resource_dir}/licenses/aom-inspection"
        VERBATIM)
endif()

target_link_libraries(BitScopeQt PRIVATE Qt6::Widgets Qt6::Concurrent)
target_compile_definitions(BitScopeQt PRIVATE QT_NO_KEYWORDS)

if(MSVC)
    target_compile_options(BitScopeQt PRIVATE /W4 /permissive-)
else()
    target_compile_options(BitScopeQt PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(BitScopeQt PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "BitScope"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.bitscope.streamanalyzer"
    OUTPUT_NAME "BitScope"
)

install(TARGETS BitScopeQt
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

qt_generate_deploy_app_script(
    TARGET BitScopeQt
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

option(BITSCOPE_BUILD_TESTS "Build BitScope smoke-test helpers" OFF)
if(BITSCOPE_BUILD_TESTS)
    enable_testing()
    qt_add_executable(BitScopeHevcOrderTest tests/hevcorder_test.cpp
        src/bitstreamparser.cpp src/frametablemodel.cpp)
    target_include_directories(BitScopeHevcOrderTest PRIVATE src)
    target_link_libraries(BitScopeHevcOrderTest PRIVATE Qt6::Widgets)
    target_compile_definitions(BitScopeHevcOrderTest PRIVATE QT_NO_KEYWORDS)
    add_test(NAME hevc-picture-order COMMAND BitScopeHevcOrderTest)
    # Exercise the actual window's signal wiring as well as decoder failures.
    get_target_property(_bitscope_ui_sources BitScopeQt SOURCES)
    list(FILTER _bitscope_ui_sources EXCLUDE REGEX "(^|/)main\\.cpp$")
    list(FILTER _bitscope_ui_sources EXCLUDE REGEX "platformwindow_(mac|win)\\.(mm|cpp)$")
    qt_add_executable(BitScopePreviewDiagnosticsTest
        tests/previewdiagnostics_test.cpp ${_bitscope_ui_sources})
    target_include_directories(BitScopePreviewDiagnosticsTest PRIVATE src)
    target_link_libraries(BitScopePreviewDiagnosticsTest PRIVATE Qt6::Widgets Qt6::Concurrent)
    target_compile_definitions(BitScopePreviewDiagnosticsTest PRIVATE QT_NO_KEYWORDS)
    add_test(NAME preview-diagnostics COMMAND BitScopePreviewDiagnosticsTest)

    qt_add_executable(BitScopeWindowChromeTest
        tests/windowchrome_test.cpp
        src/windowtitlebar.h
        src/windowtitlebar.cpp
    )
    if(WIN32)
        target_sources(BitScopeWindowChromeTest PRIVATE src/platformwindow_win.cpp)
        target_link_libraries(BitScopeWindowChromeTest PRIVATE user32)
    endif()
    target_include_directories(BitScopeWindowChromeTest PRIVATE src)
    target_link_libraries(BitScopeWindowChromeTest PRIVATE Qt6::Widgets)
    target_compile_definitions(BitScopeWindowChromeTest PRIVATE QT_NO_KEYWORDS)
    add_test(NAME window-chrome COMMAND BitScopeWindowChromeTest)

    qt_add_executable(BitScopeMacroblockLogTest
        tests/macroblocklog_test.cpp
        src/previewprovider.h
        src/previewprovider.cpp
    )
    target_include_directories(BitScopeMacroblockLogTest PRIVATE src)
    target_link_libraries(BitScopeMacroblockLogTest PRIVATE Qt6::Core Qt6::Gui)
    target_compile_definitions(BitScopeMacroblockLogTest PRIVATE QT_NO_KEYWORDS)
    add_test(NAME macroblock-log-parser COMMAND BitScopeMacroblockLogTest)

    qt_add_executable(BitScopeAv1BlockSmoke
        tests/av1blockprovider_smoke.cpp
        src/bitstreamtypes.h
        src/bitreader.h
        src/bitstreamparser.h
        src/bitstreamparser.cpp
        src/av1blockprovider.h
        src/av1blockprovider.cpp
    )
    target_include_directories(BitScopeAv1BlockSmoke PRIVATE src)
    target_link_libraries(BitScopeAv1BlockSmoke PRIVATE Qt6::Core Qt6::Gui Qt6::Concurrent)
    target_compile_definitions(BitScopeAv1BlockSmoke PRIVATE QT_NO_KEYWORDS)

    qt_add_executable(BitScopeHevcCuSmoke
        tests/hevccuprovider_smoke.cpp
        src/bitstreamtypes.h
        src/bitreader.h
        src/bitstreamparser.h
        src/bitstreamparser.cpp
        src/hevcaccess.h
        src/hevccuprovider.h
        src/hevccuprovider.cpp
    )
    target_include_directories(BitScopeHevcCuSmoke PRIVATE src)
    target_link_libraries(BitScopeHevcCuSmoke PRIVATE Qt6::Core Qt6::Gui)
    target_compile_definitions(BitScopeHevcCuSmoke PRIVATE QT_NO_KEYWORDS)

    qt_add_executable(BitScopePreviewSmoke
        tests/previewprovider_smoke.cpp
        src/bitstreamtypes.h
        src/bitreader.h
        src/bitstreamparser.h
        src/bitstreamparser.cpp
        src/hevcaccess.h
        src/previewprovider.h
        src/previewprovider.cpp
    )
    target_include_directories(BitScopePreviewSmoke PRIVATE src)
    target_link_libraries(BitScopePreviewSmoke PRIVATE Qt6::Core Qt6::Gui)
    target_compile_definitions(BitScopePreviewSmoke PRIVATE QT_NO_KEYWORDS)
endif()
