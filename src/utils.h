#pragma once

#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <functional>
#include <random>

namespace ipc2kicad {

// Unit conversion factors to mm
double unit_to_mm(const std::string& unit);

// Parse a numeric attribute, returning default if missing/invalid
double parse_double(const char* str, double default_val = 0.0);
int parse_int(const char* str, int default_val = 0);
bool parse_bool(const char* str, bool default_val = false);

// Format a double for KiCad output (6 decimal places, trailing zeros trimmed)
std::string fmt(double val);

// Generate a UUID string (v4-like, deterministic from seed or random)
std::string generate_uuid();
std::string generate_uuid_from_seed(const std::string& seed);

// Trim whitespace
std::string trim(const std::string& s);

// Case-insensitive string compare
bool iequals(const std::string& a, const std::string& b);

// Escape a string for S-expression output
std::string sexp_quote(const std::string& s);

} // namespace ipc2kicad
