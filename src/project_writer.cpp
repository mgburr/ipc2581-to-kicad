#include "project_writer.h"
#include <fstream>

namespace ipc2kicad {

bool write_project_file(const std::string& filename, const std::string& project_name) {
    std::ofstream out(filename);
    if (!out.is_open()) return false;

    // Minimal KiCad 9 project file (JSON format)
    out << "{\n"
        << "  \"meta\": {\n"
        << "    \"filename\": \"" << project_name << ".kicad_pro\",\n"
        << "    \"version\": 1\n"
        << "  },\n"
        << "  \"board\": {\n"
        << "    \"3dviewports\": [],\n"
        << "    \"design_settings\": {},\n"
        << "    \"layer_presets\": [],\n"
        << "    \"viewports\": []\n"
        << "  },\n"
        << "  \"schematic\": {\n"
        << "    \"drawing\": {},\n"
        << "    \"meta\": {\n"
        << "      \"version\": 1\n"
        << "    }\n"
        << "  },\n"
        << "  \"sheets\": [\n"
        << "    [\n"
        << "      \"e63e39d7-6ac0-4ffd-8aa3-1841a4541b55\",\n"
        << "      \"\"\n"
        << "    ]\n"
        << "  ],\n"
        << "  \"text_variables\": {}\n"
        << "}\n";

    return out.good();
}

} // namespace ipc2kicad
