#include "drill_parser.h"
#include "geometry.h"
#include "utils.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <regex>

namespace ipc2kicad {

DrillParser::DrillParser(bool verbose) : verbose_(verbose) {}

bool DrillParser::parse(const std::string& filepath, PcbModel& model) {
    std::ifstream f(filepath);
    if (!f.is_open()) {
        warn("Cannot open drill file: " + filepath);
        return false;
    }

    // Detect plated vs non-plated from filename
    std::string lower_name = filepath;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    bool is_npth = (lower_name.find("npth") != std::string::npos ||
                    lower_name.find("non-plated") != std::string::npos ||
                    lower_name.find("nonplated") != std::string::npos ||
                    lower_name.find("non_plated") != std::string::npos);

    if (verbose_) {
        log("Parsing drill file: " + filepath +
            (is_npth ? " (non-plated)" : " (plated)"));
    }

    std::map<int, DrillTool> tools;
    int current_tool = 0;
    bool in_header = false;
    bool metric = true;
    bool has_explicit_decimal = false;

    // Coordinate format: default is 2.4 for metric, 2.4 for inch
    int coord_int = 2, coord_dec = 4;
    bool leading_zero_suppression = true;

    std::string line;
    int drill_count = 0;

    while (std::getline(f, line)) {
        // Trim trailing whitespace/CR
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty()) continue;

        // Header start
        if (line == "M48" || line == "M48*") {
            in_header = true;
            continue;
        }

        // Header end
        if (line == "%" || line == "M95") {
            in_header = false;
            continue;
        }

        // Units
        if (line == "METRIC" || line.substr(0, 7) == "METRIC,") {
            metric = true;
            if (line.find(",TZ") != std::string::npos)
                leading_zero_suppression = false;  // trailing zeros suppressed = leading zeros present
            if (line.find(",LZ") != std::string::npos)
                leading_zero_suppression = true;   // leading zeros suppressed
            continue;
        }
        if (line == "INCH" || line.substr(0, 5) == "INCH,") {
            metric = false;
            if (line.find(",TZ") != std::string::npos)
                leading_zero_suppression = false;
            if (line.find(",LZ") != std::string::npos)
                leading_zero_suppression = true;
            continue;
        }

        // Format hint: FMAT,2 (format version 2 = default)
        if (line.substr(0, 5) == "FMAT,") continue;

        // ICI mode (incremental) - we only support absolute
        if (line == "ICI,ON" || line == "ICI,OFF") continue;

        // Tool definition: TnnCdiam or Tnn (with attributes)
        if (line[0] == 'T' && line.size() >= 2 && std::isdigit(line[1])) {
            // Parse tool number
            size_t i = 1;
            while (i < line.size() && std::isdigit(line[i])) i++;
            int tool_num = std::stoi(line.substr(1, i - 1));

            // Look for C (diameter)
            auto c_pos = line.find('C', i);
            if (c_pos != std::string::npos) {
                std::string diam_str;
                size_t j = c_pos + 1;
                while (j < line.size() && (std::isdigit(line[j]) || line[j] == '.' || line[j] == '-'))
                    diam_str += line[j++];

                if (!diam_str.empty()) {
                    double diam = std::stod(diam_str);
                    if (!metric) diam *= 25.4;  // convert inch to mm

                    DrillTool tool;
                    tool.number = tool_num;
                    tool.diameter = diam;
                    tools[tool_num] = tool;

                    if (verbose_) {
                        log("  Tool T" + std::to_string(tool_num) +
                            " = " + std::to_string(diam) + "mm");
                    }
                }
            } else if (!in_header) {
                // Tool selection (body)
                current_tool = tool_num;
            }
            continue;
        }

        // Skip header comments and other header commands
        if (in_header) continue;

        // M-codes in body
        if (line[0] == 'M') {
            if (line == "M30" || line == "M00") break;  // end of program
            continue;
        }

        // G-codes
        if (line[0] == 'G') {
            // G05 = drill mode (default), G00 = route mode, etc.
            // Extract any X/Y after the G code
            size_t i = 1;
            while (i < line.size() && std::isdigit(line[i])) i++;
            if (i < line.size() && (line[i] == 'X' || line[i] == 'Y')) {
                // Process the coordinate part
                line = line.substr(i);
                // Fall through to coordinate parsing below
            } else {
                continue;
            }
        }

        // Coordinate data: XnnnYnnn or X...Y...
        if (line[0] == 'X' || line[0] == 'Y') {
            double x = 0.0, y = 0.0;
            bool got_x = false, got_y = false;

            size_t pos = 0;
            while (pos < line.size()) {
                if (line[pos] == 'X') {
                    pos++;
                    std::string val;
                    if (pos < line.size() && (line[pos] == '+' || line[pos] == '-'))
                        val += line[pos++];
                    while (pos < line.size() && (std::isdigit(line[pos]) || line[pos] == '.'))
                        val += line[pos++];

                    if (val.find('.') != std::string::npos) {
                        x = std::stod(val);
                        has_explicit_decimal = true;
                    } else {
                        // Implied decimal point
                        int total = coord_int + coord_dec;
                        std::string digits;
                        bool neg = false;
                        for (char c : val) {
                            if (c == '-') neg = true;
                            else if (std::isdigit(c)) digits += c;
                        }
                        if (leading_zero_suppression) {
                            while ((int)digits.size() < total) digits = "0" + digits;
                        } else {
                            while ((int)digits.size() < total) digits += "0";
                        }
                        if ((int)digits.size() > coord_dec) {
                            std::string ip = digits.substr(0, digits.size() - coord_dec);
                            std::string dp = digits.substr(digits.size() - coord_dec);
                            x = std::stod(ip + "." + dp);
                        }
                        if (neg) x = -x;
                    }
                    got_x = true;
                } else if (line[pos] == 'Y') {
                    pos++;
                    std::string val;
                    if (pos < line.size() && (line[pos] == '+' || line[pos] == '-'))
                        val += line[pos++];
                    while (pos < line.size() && (std::isdigit(line[pos]) || line[pos] == '.'))
                        val += line[pos++];

                    if (val.find('.') != std::string::npos) {
                        y = std::stod(val);
                        has_explicit_decimal = true;
                    } else {
                        int total = coord_int + coord_dec;
                        std::string digits;
                        bool neg = false;
                        for (char c : val) {
                            if (c == '-') neg = true;
                            else if (std::isdigit(c)) digits += c;
                        }
                        if (leading_zero_suppression) {
                            while ((int)digits.size() < total) digits = "0" + digits;
                        } else {
                            while ((int)digits.size() < total) digits += "0";
                        }
                        if ((int)digits.size() > coord_dec) {
                            std::string ip = digits.substr(0, digits.size() - coord_dec);
                            std::string dp = digits.substr(digits.size() - coord_dec);
                            y = std::stod(ip + "." + dp);
                        }
                        if (neg) y = -y;
                    }
                    got_y = true;
                } else {
                    pos++;
                }
            }

            if (!got_x && !got_y) continue;

            // Convert to mm
            if (!metric) {
                x *= 25.4;
                y *= 25.4;
            }

            // Get tool diameter
            double drill_diam = 0.0;
            auto it = tools.find(current_tool);
            if (it != tools.end()) {
                drill_diam = it->second.diameter;
            }

            if (drill_diam <= 0.0) continue; // no valid tool

            // Convert Y-up to Y-down (Gerber/Excellon uses Y-up like IPC)
            Point kp = {x, -y};

            if (is_npth) {
                // Non-plated through hole
                DrillHole dh;
                dh.position = kp;
                dh.diameter = drill_diam;
                dh.plated = false;
                model.drills.push_back(dh);
            } else {
                // Plated through hole -> Via
                Via via;
                via.position = kp;
                via.drill = drill_diam;
                via.diameter = drill_diam + 0.3; // estimated annular ring
                via.start_layer = "F.Cu";
                via.end_layer = "B.Cu";
                via.net_id = 0;
                model.vias.push_back(via);

                drill_count++;
            }
        }
    }

    log("Parsed " + std::to_string(drill_count) + " plated drill hits from " + filepath);
    return true;
}

void DrillParser::log(const std::string& msg) {
    if (verbose_) std::cerr << "DrillParser: " << msg << "\n";
}

void DrillParser::warn(const std::string& msg) {
    warnings_.push_back(msg);
    std::cerr << "Warning: " << msg << "\n";
}

} // namespace ipc2kicad
