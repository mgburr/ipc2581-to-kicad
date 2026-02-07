#pragma once

#include "pcb_model.h"
#include "model_mapper.h"
#include <string>
#include <ostream>

namespace ipc2kicad {

enum class KiCadVersion {
    V7,  // KiCad 7.x format
    V8,  // KiCad 8.x format
    V9   // KiCad 9.x format
};

struct WriterOptions {
    KiCadVersion version = KiCadVersion::V9;
    bool verbose = false;
};

class KicadWriter {
public:
    explicit KicadWriter(const WriterOptions& opts = {});

    // Write the model to a .kicad_pcb file. Returns true on success.
    bool write(const std::string& filename, const PcbModel& model);

    // Write the model to an output stream.
    bool write(std::ostream& out, const PcbModel& model);

private:
    WriterOptions opts_;
    ModelMapper model_mapper_;
    int indent_ = 0;

    // Version helpers
    bool has_uuids() const { return opts_.version == KiCadVersion::V8 || opts_.version == KiCadVersion::V9; }
    std::string uuid_fmt(const std::string& seed) const;
    int layer_id(int v78_id) const;

    // Section writers
    void write_header(std::ostream& out);
    void write_general(std::ostream& out, const PcbModel& model);
    void write_paper(std::ostream& out);
    void write_layers(std::ostream& out, const PcbModel& model);
    void write_setup(std::ostream& out, const PcbModel& model);
    void write_stackup(std::ostream& out, const PcbModel& model);
    void write_nets(std::ostream& out, const PcbModel& model);
    void write_footprints(std::ostream& out, const PcbModel& model);
    void write_footprint(std::ostream& out, const PcbModel& model,
                         const ComponentInstance& comp, const Footprint& fp);
    void write_pad(std::ostream& out, const PadDef& pad,
                   const ComponentInstance& comp, const PcbModel& model);
    void write_traces(std::ostream& out, const PcbModel& model);
    void write_vias(std::ostream& out, const PcbModel& model);
    void write_zones(std::ostream& out, const PcbModel& model);
    void write_outline(std::ostream& out, const PcbModel& model);
    void write_graphics(std::ostream& out, const PcbModel& model);

    // Formatting helpers
    std::string ind() const;
    std::string uuid() const;
    std::string uuid_from(const std::string& seed) const;

    void log(const std::string& msg);
};

} // namespace ipc2kicad
