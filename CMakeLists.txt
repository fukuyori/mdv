cmake_minimum_required(VERSION 3.16)

project(mdv VERSION 0.6.2 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(WIN32)
    enable_language(RC)
endif()

set(MDV_MIN_QT_VERSION 6.10.3)
find_package(Qt6 ${MDV_MIN_QT_VERSION} REQUIRED
    COMPONENTS Widgets WebEngineWidgets WebChannel Network)

add_library(md4c STATIC
    third_party/md4c/md4c.c
    third_party/md4c/md4c-html.c
    third_party/md4c/entity.c
)
target_include_directories(md4c PUBLIC third_party/md4c)

if(APPLE)
    add_executable(mdv MACOSX_BUNDLE
        src/codex_log.cpp
        src/main.cpp
        resources/mdv.qrc
        resources/macos/mdv.icns
    )
    set_source_files_properties(resources/macos/mdv.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
elseif(WIN32)
    add_executable(mdv
        src/codex_log.cpp
        src/main.cpp
        resources/mdv.qrc
        resources/windows/mdv.rc
    )
else()
    add_executable(mdv
        src/codex_log.cpp
        src/main.cpp
        resources/mdv.qrc
    )
endif()

target_link_libraries(mdv PRIVATE
    Qt6::Widgets
    Qt6::WebEngineWidgets
    Qt6::WebChannel
    Qt6::Network
    md4c
)
target_compile_definitions(mdv PRIVATE MDV_VERSION="${PROJECT_VERSION}")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mdv PRIVATE
        -Wall -Wextra -Wpedantic
        -fstack-protector-strong
    )
    target_compile_options(md4c PRIVATE -fstack-protector-strong)
    target_compile_definitions(mdv PRIVATE
        $<$<CONFIG:Release>:_FORTIFY_SOURCE=3>
    )
    target_compile_definitions(md4c PRIVATE
        $<$<CONFIG:Release>:_FORTIFY_SOURCE=3>
    )
    if(UNIX AND NOT APPLE)
        target_link_options(mdv PRIVATE -Wl,-z,relro -Wl,-z,now)
    endif()
endif()

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

qt_finalize_executable(mdv)

if(BUILD_TESTING)
    add_executable(mdv_security_test tests/security_test.cpp)
    target_link_libraries(mdv_security_test PRIVATE md4c)
    add_test(NAME mdv_security_test COMMAND mdv_security_test)

    add_executable(mdv_codex_log_test
        tests/codex_log_test.cpp
        src/codex_log.cpp
    )
    target_include_directories(mdv_codex_log_test PRIVATE src)
    target_link_libraries(mdv_codex_log_test PRIVATE Qt6::Core)
    add_test(NAME mdv_codex_log_test COMMAND mdv_codex_log_test)
endif()
