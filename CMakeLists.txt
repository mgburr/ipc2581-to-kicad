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
    src/json_import.cpp
    src/geometry.cpp
    src/utils.cpp
    src/schematic_writer.cpp
    src/project_writer.cpp
    src/vrml_writer.cpp
)

target_include_directories(ipc2581-to-kicad PRIVATE
    src
    third_party/pugixml
    third_party
)

target_link_libraries(ipc2581-to-kicad PRIVATE pugixml)

# Install
install(TARGETS ipc2581-to-kicad DESTINATION bin)
install(FILES
    odb/__init__.py
    odb/pcb_model.py
    odb/odb_parser.py
    odb/utils.py
    odb/odb_to_json.py
    DESTINATION share/ipc2581-to-kicad/odb
)
