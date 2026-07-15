// Level registry and custom levels.
//
// A level is just a name and an integer value. Higher value means higher
// verbosity. Users can add their own levels and place them anywhere in the
// order by picking a value. The registry keeps names alive so a Level's name
// pointer stays valid for the life of the process.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "internal.h"
#include "slog/slog.h"

namespace slog {
namespace {

struct Registry {
    std::mutex                               mu;
    std::deque<std::string>                  storage;  // stable name storage
    std::vector<std::pair<const char*, int>> entries;  // name -> value
};

// One registry for the whole process, with the built-in levels pre-loaded.
Registry& registry() {
    static Registry       r;
    static std::once_flag once;
    std::call_once(once, [] {
        r.entries.push_back({"ERROR", SLOG_LEVEL_ERROR});
        r.entries.push_back({"WARNING", SLOG_LEVEL_WARNING});
        r.entries.push_back({"INFO", SLOG_LEVEL_INFO});
        r.entries.push_back({"DEBUG", SLOG_LEVEL_DEBUG});
    });
    return r;
}

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

Level add_level(const char* name, int value) {
    Registry&                   r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    // Keep the name in stable storage so the returned pointer stays valid.
    r.storage.emplace_back(name);
    const char* stored = r.storage.back().c_str();
    r.entries.push_back({stored, value});
    return Level{value, stored};
}

namespace detail {

bool parse_level(const std::string& text, int& value_out) {
    std::string t = to_lower(text);
    // Trim surrounding spaces.
    const std::size_t a = t.find_first_not_of(" \t");
    const std::size_t b = t.find_last_not_of(" \t");
    if (a == std::string::npos)
        return false;
    t = t.substr(a, b - a + 1);

    if (t == "inherit" || t == "default") {
        value_out = kInherit;
        return true;
    }

    Registry& r = registry();
    {
        std::lock_guard<std::mutex> lock(r.mu);
        for (const auto& e : r.entries) {
            if (to_lower(e.first) == t) {
                value_out = e.second;
                return true;
            }
        }
    }

    // Not a name. Try a plain integer, so config can place a level by value.
    char*      end = nullptr;
    const long v   = std::strtol(t.c_str(), &end, 10);
    if (end != t.c_str() && *end == '\0') {
        value_out = static_cast<int>(v);
        return true;
    }
    return false;
}

std::string level_name(int value) {
    if (value == kInherit)
        return "inherit";
    if (value == kOff)
        return "off";
    Registry& r = registry();
    {
        std::lock_guard<std::mutex> lock(r.mu);
        for (const auto& e : r.entries) {
            if (e.second == value)
                return e.first;
        }
    }
    return std::to_string(value);
}

}  // namespace detail
}  // namespace slog
