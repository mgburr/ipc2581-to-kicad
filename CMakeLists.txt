cmake_minimum_required(VERSION 3.14)
project(ipc2581-to-kicad VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# pugixml (vendored)
add_library(pugixml STATIC
    third_party/pugixml/pugixml.cpp
)
target_include_directories(pugixml PUBLIC third_party/pugixml)

# Main executable
add_executable(ipc2581-to-kicad
    src/main.cpp
    src/ipc2581_parser.cpp
    src/kicad_writer.cpp
    src/model_mapper.cpp
    src/json_export.cpp
    src/geometry.cpp
    src/utils.cpp
)

target_include_directories(ipc2581-to-kicad PRIVATE
    src
    third_party/pugixml
)

target_link_libraries(ipc2581-to-kicad PRIVATE pugixml)

# Install
install(TARGETS ipc2581-to-kicad DESTINATION bin)
