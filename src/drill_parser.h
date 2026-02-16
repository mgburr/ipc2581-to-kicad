#pragma once

#include "pcb_model.h"
#include <string>
#include <vector>
#include <map>

namespace ipc2kicad {

class DrillParser {
public:
    explicit DrillParser(bool verbose = false);

    // Parse an Excellon drill file and add vias to model
    bool parse(const std::string& filepath, PcbModel& model);

    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    bool verbose_;
    std::vector<std::string> warnings_;

    struct DrillTool {
        int number = 0;
        double diameter = 0.0;  // mm
    };

    void log(const std::string& msg);
    void warn(const std::string& msg);
};

} // namespace ipc2kicad
