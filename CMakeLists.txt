cmake_minimum_required(VERSION 3.16)

project(mdv VERSION 0.1.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(WIN32)
    enable_language(RC)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets WebEngineWidgets WebChannel)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets WebEngineWidgets WebChannel)

add_library(md4c STATIC
    third_party/md4c/md4c.c
    third_party/md4c/md4c-html.c
    third_party/md4c/entity.c
)
target_include_directories(md4c PUBLIC third_party/md4c)

if(APPLE)
    add_executable(mdv MACOSX_BUNDLE
        src/main.cpp
        resources/mdv.qrc
        resources/macos/mdv.icns
    )
    set_source_files_properties(resources/macos/mdv.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
elseif(WIN32)
    add_executable(mdv
        src/main.cpp
        resources/mdv.qrc
        resources/windows/mdv.rc
    )
else()
    add_executable(mdv
        src/main.cpp
        resources/mdv.qrc
    )
endif()

target_link_libraries(mdv PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::WebEngineWidgets
    Qt${QT_VERSION_MAJOR}::WebChannel
    md4c
)
target_compile_definitions(mdv PRIVATE MDV_VERSION="${PROJECT_VERSION}")

set_target_properties(mdv PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.fukuyori.mdv"
    MACOSX_BUNDLE_BUNDLE_NAME "mdv"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "mdv.icns"
)

if(WIN32)
    set_target_properties(mdv PROPERTIES WIN32_EXECUTABLE ON)
endif()

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(mdv)
endif()
