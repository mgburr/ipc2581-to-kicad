#pragma once

#include "pcb_model.h"
#include <ostream>

namespace ipc2kicad {

// Serialize a PcbModel to JSON format for use by the Python viewer tools.
void write_json(std::ostream& out, const PcbModel& model);

} // namespace ipc2kicad
