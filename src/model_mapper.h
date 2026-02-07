#pragma once

#include <string>

namespace ipc2kicad {

class ModelMapper {
public:
    // Returns a 3D model path (e.g. "Resistor_SMD.3dshapes/R_0603_1608Metric.step")
    // or empty string if no match is found.
    std::string lookup(const std::string& package_name) const;

private:
    std::string try_direct(const std::string& name) const;
    std::string try_ipc7351(const std::string& name) const;
    std::string metric_suffix(int l_tenth_mm, int w_tenth_mm) const;
};

} // namespace ipc2kicad
