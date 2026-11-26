cmake_minimum_required(VERSION 3.21)

project(BitScopeQt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Concurrent)
qt_standard_project_setup()
# Some managed macOS installations protect CMake's compiler probe source.
# MOC does not need those optional predefines for this project.
set(CMAKE_AUTOMOC_COMPILER_PREDEFINES OFF)

qt_add_executable(BitScopeQt
    src/main.cpp
    src/bitstreamtypes.h
    src/bitreader.h
    src/bitstreamparser.h
    src/bitstreamparser.cpp
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

set(BITSCOPE_HEVC_ANALYZER "" CACHE FILEPATH
    "Optional HM_180 analyzer executable to bundle for HEVC CU-tree extraction")
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
endif()
