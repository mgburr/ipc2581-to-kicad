#include "model_mapper.h"

#include <regex>
#include <unordered_map>

namespace ipc2kicad {

// Imperial-to-metric size table: imperial code -> { metric_LW, KiCad suffix }
struct SizeEntry {
    int l_tenth;   // length in 0.1mm
    int w_tenth;   // width  in 0.1mm
    const char* suffix; // e.g. "_0402_1005Metric"
};

static const SizeEntry size_table[] = {
    {10, 5,  "_0402_1005Metric"},
    {16, 8,  "_0603_1608Metric"},
    {20, 12, "_0805_2012Metric"},
    {32, 16, "_1206_3216Metric"},
    {32, 25, "_1210_3225Metric"},
    {45, 32, "_1812_4532Metric"},
    {63, 32, "_2512_6332Metric"},
};

static const char* find_suffix_by_imperial(const std::string& imp) {
    static const std::unordered_map<std::string, const char*> imp_map = {
        {"0402", "_0402_1005Metric"},
        {"0603", "_0603_1608Metric"},
        {"0805", "_0805_2012Metric"},
        {"1206", "_1206_3216Metric"},
        {"1210", "_1210_3225Metric"},
        {"1812", "_1812_4532Metric"},
        {"2512", "_2512_6332Metric"},
    };
    auto it = imp_map.find(imp);
    return it != imp_map.end() ? it->second : nullptr;
}

// Find the closest metric suffix given dimensions in 0.1mm units
static const char* find_suffix_by_metric(int l, int w) {
    int best_dist = 9999;
    const char* best = nullptr;
    for (auto& e : size_table) {
        int dist = std::abs(l - e.l_tenth) + std::abs(w - e.w_tenth);
        if (dist < best_dist) {
            best_dist = dist;
            best = e.suffix;
        }
    }
    // Only match if reasonably close (within 3 units of 0.1mm per dimension)
    return (best && best_dist <= 6) ? best : nullptr;
}

// Map component type prefix to library/3dshapes directory and filename prefix
struct TypeInfo {
    const char* lib;    // e.g. "Resistor_SMD.3dshapes"
    const char* prefix; // e.g. "R"
};

static TypeInfo type_for_prefix(const std::string& p) {
    if (p == "R" || p == "RES")  return {"Resistor_SMD.3dshapes",  "R"};
    if (p == "C" || p == "CAP")  return {"Capacitor_SMD.3dshapes", "C"};
    if (p == "L" || p == "IND")  return {"Inductor_SMD.3dshapes",  "L"};
    if (p == "LED")              return {"LED_SMD.3dshapes",        "LED"};
    if (p == "D" || p == "DIO")  return {"Diode_SMD.3dshapes",     "D"};
    return {nullptr, nullptr};
}

// --- Direct lookup: exact KiCad-style names ---

std::string ModelMapper::try_direct(const std::string& name) const {
    // Pattern: PREFIX_IMPERIAL  e.g. R_0603, C_0805, LED_0603, L_1206
    static const std::regex re_simple(R"(^(R|C|L|LED|D)_(\d{4})$)");
    std::smatch m;
    if (std::regex_match(name, m, re_simple)) {
        auto ti = type_for_prefix(m[1].str());
        if (ti.lib) {
            auto suffix = find_suffix_by_imperial(m[2].str());
            if (suffix) {
                return std::string(ti.lib) + "/" + ti.prefix + suffix + ".step";
            }
        }
    }

    // SOT-23 family
    if (name == "SOT-23" || name == "SOT-23-3")
        return "Package_TO_SOT_SMD.3dshapes/SOT-23.step";
    if (name == "SOT-23-5")
        return "Package_TO_SOT_SMD.3dshapes/SOT-23-5.step";
    if (name == "SOT-23-6")
        return "Package_TO_SOT_SMD.3dshapes/SOT-23-6.step";

    // SOT-223
    if (name == "SOT-223" || name == "SOT-223-3")
        return "Package_TO_SOT_SMD.3dshapes/SOT-223-3_TabPin2.step";

    // QFN / DFN common packages
    if (name == "QFN-16" || name == "QFN-16-1EP_3x3mm_P0.5mm")
        return "Package_DFN_QFN.3dshapes/QFN-16-1EP_3x3mm_P0.5mm.step";

    // Tactile / push button switches (manufacturer part numbers)
    // E-Switch TL1014 series — 6x6mm tactile switch
    if (name.rfind("TL1014", 0) == 0 || name.rfind("TL3301", 0) == 0)
        return "Button_Switch_SMD.3dshapes/SW_Push_1P1T_NO_E-Switch_TL3301NxxxxxG.step";
    // Generic 6x6mm tactile switches
    if (name.rfind("SW_Push_6x6", 0) == 0 || name.rfind("TACT_6", 0) == 0)
        return "Button_Switch_SMD.3dshapes/SW_Push_1TS009xxxx-xxxx-xxxx_6x6x5mm.step";
    // Hirose FH12 FPC connectors: extract pin count from name
    {
        static const std::regex re_fh12(R"(FH12[\-]?(\d+)S)", std::regex::icase);
        std::smatch fm;
        std::string search_name = name;
        // Strip CON- prefix for matching
        if (search_name.rfind("CON-", 0) == 0)
            search_name = search_name.substr(4);
        if (std::regex_search(search_name, fm, re_fh12)) {
            std::string pins = fm[1].str();
            return "Connector_FFC-FPC.3dshapes/Hirose_FH12-" + pins +
                   "S-0.5SH_1x" + pins + "-1MP_P0.50mm_Horizontal.step";
        }
    }

    return {};
}

// --- IPC-7351 dimension parsing ---

std::string ModelMapper::try_ipc7351(const std::string& name) const {
    // Pattern: (CAP|RES|IND|LED|DIO)C?(\d{2})(\d{2})X\d+N?
    static const std::regex re_ipc(R"(^(CAP|RES|IND|LED|DIO)C?(\d{2})(\d{2})X\d+N?$)");
    std::smatch m;
    if (!std::regex_match(name, m, re_ipc))
        return {};

    std::string type_code = m[1].str();
    int l = std::stoi(m[2].str());
    int w = std::stoi(m[3].str());

    auto ti = type_for_prefix(type_code);
    if (!ti.lib)
        return {};

    auto suffix = find_suffix_by_metric(l, w);
    if (!suffix)
        return {};

    return std::string(ti.lib) + "/" + ti.prefix + suffix + ".step";
}

// Build a model path from a type prefix (RES/CAP/etc.) and a bare imperial code (0402/0603/etc.)
static std::string try_bare_imperial(const std::string& type_prefix, const std::string& imperial) {
    auto ti = type_for_prefix(type_prefix);
    if (!ti.lib)
        return {};
    auto suffix = find_suffix_by_imperial(imperial);
    if (!suffix)
        return {};
    return std::string(ti.lib) + "/" + ti.prefix + suffix + ".step";
}

std::string ModelMapper::lookup(const std::string& package_name) const {
    if (package_name.empty())
        return {};

    // Strip trailing dedup suffixes like _1, _2 (but not 4-digit imperial codes)
    static const std::regex re_dedup(R"(_\d{1,2}$)");
    std::string name = std::regex_replace(package_name, re_dedup, "");

    // 1. Direct lookup
    std::string result = try_direct(name);
    if (!result.empty())
        return result;

    // 2. IPC-7351 parsing
    result = try_ipc7351(name);
    if (!result.empty())
        return result;

    // 3. Prefix stripping: remove RES-, CAP-, LED-, IND-, DIO- and retry
    static const std::regex re_prefix(R"(^(RES|CAP|LED|IND|DIO)[-_](.+)$)");
    std::smatch pm;
    if (std::regex_match(name, pm, re_prefix)) {
        std::string type_hint = pm[1].str();
        std::string rest = pm[2].str();

        result = try_direct(rest);
        if (!result.empty())
            return result;
        result = try_ipc7351(rest);
        if (!result.empty())
            return result;

        // Bare imperial code after prefix strip: RES-0402 -> type=RES, code=0402
        static const std::regex re_bare_imp(R"(^\d{4}$)");
        if (std::regex_match(rest, re_bare_imp)) {
            result = try_bare_imperial(type_hint, rest);
            if (!result.empty())
                return result;
        }
    }

    // 4. No match
    return {};
}

} // namespace ipc2kicad
