#pragma once

#include "pcb_model.h"
#include <istream>
#include <string>

namespace ipc2kicad {

// Read JSON (as produced by json_export.cpp or odb_to_json.py) into a PcbModel.
// Returns true on success, false on parse error.
bool read_json(std::istream& in, PcbModel& model);

// Convenience: read from a JSON string.
bool read_json(const std::string& json_text, PcbModel& model);

} // namespace ipc2kicad
