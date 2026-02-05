#include "utils.h"
#include <cstring>
#include <cctype>
#include <functional>
#include <sstream>
#include <iomanip>
#include <random>
#include <chrono>

namespace ipc2kicad {

double unit_to_mm(const std::string& unit) {
    if (unit == "MM" || unit == "MILLIMETER") return 1.0;
    if (unit == "INCH") return 25.4;
    if (unit == "MIL" || unit == "THOU") return 0.0254;
    if (unit == "MICRON") return 0.001;
    return 1.0; // default mm
}

double parse_double(const char* str, double default_val) {
    if (!str || str[0] == '\0') return default_val;
    try {
        return std::stod(str);
    } catch (...) {
        return default_val;
    }
}

int parse_int(const char* str, int default_val) {
    if (!str || str[0] == '\0') return default_val;
    try {
        return std::stoi(str);
    } catch (...) {
        return default_val;
    }
}

bool parse_bool(const char* str, bool default_val) {
    if (!str || str[0] == '\0') return default_val;
    std::string s(str);
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s == "true" || s == "yes" || s == "1") return true;
    if (s == "false" || s == "no" || s == "0") return false;
    return default_val;
}

std::string fmt(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << val;
    std::string s = oss.str();
    // Trim trailing zeros after decimal point
    if (s.find('.') != std::string::npos) {
        size_t last_nonzero = s.find_last_not_of('0');
        if (last_nonzero != std::string::npos && s[last_nonzero] == '.') {
            s.erase(last_nonzero); // remove the dot too
        } else {
            s.erase(last_nonzero + 1);
        }
    }
    // Avoid "-0"
    if (s == "-0") s = "0";
    return s;
}

static std::mt19937_64& get_rng() {
    static std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return rng;
}

static std::string format_uuid(uint64_t a, uint64_t b) {
    char buf[40];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        (unsigned)(a >> 32),
        (unsigned)((a >> 16) & 0xFFFF),
        (unsigned)(a & 0xFFFF) | 0x4000,  // version 4
        (unsigned)((b >> 48) & 0x3FFF) | 0x8000, // variant
        (unsigned long long)(b & 0xFFFFFFFFFFFFULL));
    return std::string(buf);
}

std::string generate_uuid() {
    auto& rng = get_rng();
    return format_uuid(rng(), rng());
}

std::string generate_uuid_from_seed(const std::string& seed) {
    std::hash<std::string> hasher;
    uint64_t h1 = hasher(seed);
    uint64_t h2 = hasher(seed + "_2");
    return format_uuid(h1, h2);
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

std::string sexp_quote(const std::string& s) {
    // If string contains spaces, quotes, or parens, wrap in quotes and escape
    bool needs_quoting = s.empty();
    for (char c : s) {
        if (c == ' ' || c == '(' || c == ')' || c == '"' || c == '\\') {
            needs_quoting = true;
            break;
        }
    }
    if (!needs_quoting) return s;

    std::string result = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') result += '\\';
        result += c;
    }
    result += '"';
    return result;
}

} // namespace ipc2kicad
