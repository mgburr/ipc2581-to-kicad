#pragma once

#include <string>

namespace ipc2kicad {

// Write a minimal .kicad_pro project file that links the PCB and schematic.
bool write_project_file(const std::string& filename, const std::string& project_name);

} // namespace ipc2kicad
