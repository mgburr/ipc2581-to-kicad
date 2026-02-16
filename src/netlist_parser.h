#pragma once

#include "pcb_model.h"
#include <string>
#include <vector>

namespace ipc2kicad {

struct NetlistRecord {
    std::string net_name;
    std::string component_ref;
    std::string pin;
    Point position;        // mm, KiCad coords (Y-down)
    double hole_diameter;  // mm, 0 for SMD
};

class NetlistParser {
public:
    explicit NetlistParser(bool verbose = false);

    // Parse an IPC-D-356 netlist file and assign nets to model
    bool parse(const std::string& filepath, PcbModel& model);

    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    bool verbose_;
    std::vector<std::string> warnings_;

    void apply_to_model(const std::vector<NetlistRecord>& records, PcbModel& model);

    void log(const std::string& msg);
    void warn(const std::string& msg);
};

} // namespace ipc2kicad
