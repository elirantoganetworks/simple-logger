// Configuration: read the file and the environment, and fold key/value pairs
// into the resolved settings.
//
// All three config sources (file, env, API) share one key vocabulary. This file
// turns the file and the environment into that key space and applies a layer of
// keys onto a Settings object. The caller applies layers in precedence order
// (file, then env, then API), so a later layer wins by simple overwrite.

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "internal.h"

namespace slog {
namespace detail {
namespace {

std::string trim(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    const std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// A '#' starts a comment only at the line start or after a space or tab. That
// keeps the %# token usable inside a format pattern (there the '#' follows a '%').
std::string strip_comment(const std::string& line) {
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t'))
            return line.substr(0, i);
    }
    return line;
}

bool parse_bool(const std::string& v, bool& out) {
    std::string t = trim(v);
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "true" || t == "1" || t == "on" || t == "yes") {
        out = true;
        return true;
    }
    if (t == "false" || t == "0" || t == "off" || t == "no") {
        out = false;
        return true;
    }
    return false;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string              cur;
    std::istringstream       in(s);
    while (std::getline(in, cur, sep)) {
        const std::string t = trim(cur);
        if (!t.empty())
            out.push_back(t);
    }
    return out;
}

// Turn an "off | on | <level>" output value into enabled + level.
void apply_output(const std::string& key, const std::string& v, bool& enabled, int& level) {
    const std::string t = trim(v);
    if (t == "off") {
        enabled = false;
        return;
    }
    enabled = true;
    if (t == "on" || t.empty()) {
        level = kInherit;
        return;
    }
    int parsed = 0;
    if (parse_level(t, parsed))
        level = parsed;
    else
        record(errc::config_bad_value, 0, (key + "=" + t).c_str());
}

// Parse a boolean value, recording a bad one instead of silently keeping the old.
void apply_bool(const std::string& key, const std::string& val, bool& out) {
    if (!parse_bool(val, out))
        record(errc::config_bad_value, 0, (key + "=" + trim(val)).c_str());
}

}  // namespace

Kv read_config_file(const std::string& path) {
    Kv kv;
    if (path.empty())
        return kv;  // no config file was requested, so this is not an error
    std::ifstream in(path);
    if (!in) {
        record(errc::config_file_read, errno, path.c_str());
        return kv;
    }
    std::string line;
    while (std::getline(in, line)) {
        const std::string body = strip_comment(line);
        const std::size_t eq   = body.find('=');
        if (eq == std::string::npos) {
            if (!trim(body).empty())
                record(errc::config_bad_value, 0, "config line without '='");
            continue;
        }
        const std::string key = trim(body.substr(0, eq));
        const std::string val = trim(body.substr(eq + 1));
        if (!key.empty())
            kv[key] = val;
    }
    return kv;
}

std::string find_config_path() {
    const char* env = std::getenv("SLOG_CONFIG");
    if (env != nullptr && env[0] != '\0')
        return env;
    if (::access("simplelog.conf", R_OK) == 0)
        return "simplelog.conf";
    return "";
}

Kv read_env() {
    Kv   kv;
    auto get = [](const char* name) -> const char* {
        const char* v = std::getenv(name);
        return (v != nullptr && v[0] != '\0') ? v : nullptr;
    };

    // The verbosity var carries the global level and per-module levels in one
    // string, e.g. "warning,net=debug,db=info".
    if (const char* v = get("SLOG_VERBOSITY")) {
        bool have_global = false;
        for (const std::string& tok : split(v, ',')) {
            const std::size_t eq = tok.find('=');
            if (eq == std::string::npos) {
                if (!have_global) {
                    kv["verbosity"] = tok;
                    have_global     = true;
                }
            } else {
                kv["module." + trim(tok.substr(0, eq))] = trim(tok.substr(eq + 1));
            }
        }
    }

    struct Map {
        const char* env;
        const char* key;
    };
    static const Map maps[] = {
        {"SLOG_DISABLE", "disable"},
        {"SLOG_DIR", "dir"},
        {"SLOG_TAG", "tag"},
        {"SLOG_FILE", "file"},
        {"SLOG_FILE_PER_MODULE", "file.per_module"},
        {"SLOG_FILE_NAME", "file.name"},
        {"SLOG_STDOUT", "stdout"},
        {"SLOG_STDOUT_ONLY", "stdout.only"},
        {"SLOG_STDOUT_MODULES", "stdout.modules"},
        {"SLOG_FORMAT", "format"},
        {"SLOG_TIME", "time"},
        {"SLOG_ELAPSED", "elapsed"},
        {"SLOG_COLOR", "color"},
        {"SLOG_FLUSH", "flush"},
        {"SLOG_RETAIN_RUNS", "retain.runs"},
        {"SLOG_RETAIN_DAYS", "retain.days"},
    };
    // NO_COLOR is a wide convention: if set, turn color off. An explicit
    // SLOG_COLOR still wins, so it is read after this.
    if (get("NO_COLOR"))
        kv["color"] = "never";
    for (const Map& m : maps) {
        if (const char* v = get(m.env))
            kv[m.key] = v;
    }
    return kv;
}

void apply_kv(Settings& s, const Kv& kv) {
    for (const auto& kvp : kv) {
        const std::string& key = kvp.first;
        const std::string& val = kvp.second;

        if (key.rfind("module.", 0) == 0) {
            const std::string module = key.substr(7);
            int               level  = 0;
            if (!parse_level(val, level))
                record(errc::config_bad_value, 0, (key + "=" + val).c_str());
            else if (level == kInherit)
                s.module_level.erase(module);  // "inherit" means no override
            else
                s.module_level[module] = level;
            continue;
        }

        if (key == "disable") {
            apply_bool(key, val, s.disabled);
        } else if (key == "verbosity") {
            int level = 0;
            if (parse_level(val, level) && level != kInherit)
                s.global_level = level;
            else
                record(errc::config_bad_value, 0, ("verbosity=" + val).c_str());
        } else if (key == "dir") {
            s.log_dir = val;
        } else if (key == "tag") {
            s.run_tag = val;
        } else if (key == "file") {
            apply_output(key, val, s.file_enabled, s.file_level);
        } else if (key == "file.per_module") {
            apply_bool(key, val, s.file_per_module);
        } else if (key == "file.name") {
            s.file_name = val;
        } else if (key == "stdout") {
            apply_output(key, val, s.stdout_enabled, s.stdout_level);
        } else if (key == "stdout.only") {
            apply_bool(key, val, s.stdout_only);
        } else if (key == "stdout.modules") {
            s.stdout_modules = split(val, ',');
        } else if (key == "format") {
            s.format = val;
        } else if (key == "time") {
            apply_bool(key, val, s.enable_time);
        } else if (key == "elapsed") {
            apply_bool(key, val, s.enable_elapsed);
        } else if (key == "color") {
            s.color = val;
        } else if (key == "flush") {
            int level = 0;
            if (parse_level(val, level) && level != kInherit)
                s.flush_level = level;
            else
                record(errc::config_bad_value, 0, ("flush=" + val).c_str());
        } else if (key == "retain.runs") {
            s.retain_runs = std::atoi(val.c_str());
        } else if (key == "retain.days") {
            s.retain_days = std::atoi(val.c_str());
        } else {
            record(errc::config_unknown_key, 0, key.c_str());
        }
    }
}

}  // namespace detail
}  // namespace slog
