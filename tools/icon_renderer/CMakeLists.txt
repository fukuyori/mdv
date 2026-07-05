cmake_minimum_required(VERSION 3.16)

project(mdv_icon_renderer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Gui Svg)

add_executable(mdv_icon_renderer
    main.cpp
)

target_link_libraries(mdv_icon_renderer PRIVATE Qt6::Gui Qt6::Svg)
