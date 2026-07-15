// Prefix pattern parsing and line assembly.
//
// The prefix is a small pattern like "[%L][%f]: %m". It is parsed once into a
// token list and then rendered per record. Rendering never allocates: it writes
// into a caller-provided buffer and truncates to keep one line at or under the
// atomic-write size.

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

#include "internal.h"
#include "slog/slog.h"

namespace slog {
namespace detail {
namespace {

// The ANSI color for a level tag, or "" for no color. Only the built-in levels
// are colored; custom levels stay plain.
const char* color_for(int value) {
    switch (value) {
        case SLOG_LEVEL_ERROR:
            return "\033[31m";  // red
        case SLOG_LEVEL_WARNING:
            return "\033[33m";  // yellow
        case SLOG_LEVEL_INFO:
            return "\033[32m";  // green
        case SLOG_LEVEL_DEBUG:
            return "\033[36m";  // cyan
        default:
            return "";
    }
}
const char* kReset = "\033[0m";

Field field_for(char c) {
    switch (c) {
        case 'L':
            return Field::Level;
        case 'f':
            return Field::Func;
        case 'm':
            return Field::Msg;
        case 'M':
            return Field::Module;
        case 't':
            return Field::Time;
        case 'e':
            return Field::Elapsed;
        case 'p':
            return Field::Pid;
        case 'T':
            return Field::Tid;
        case 'F':
            return Field::File;
        case '#':
            return Field::Line;
        default:
            return Field::Literal;  // unknown, kept as literal text
    }
}

}  // namespace

std::unique_ptr<FormatSpec> parse_format(const std::string& pattern, bool add_time,
                                         bool add_elapsed, std::int64_t start_epoch_ns) {
    // The convenience toggles just prepend the field token if it is missing, so
    // the pattern stays the one source of layout.
    std::string p;
    if (add_time && pattern.find("%t") == std::string::npos)
        p += "%t ";
    if (add_elapsed && pattern.find("%e") == std::string::npos)
        p += "%e ";
    p += pattern;

    auto spec            = std::make_unique<FormatSpec>();
    spec->start_epoch_ns = start_epoch_ns;

    std::string literal;
    auto        flush_literal = [&] {
        if (!literal.empty()) {
            spec->tokens.push_back({Field::Literal, literal});
            literal.clear();
        }
    };

    for (std::size_t i = 0; i < p.size(); ++i) {
        if (p[i] != '%' || i + 1 >= p.size()) {
            literal.push_back(p[i]);
            continue;
        }
        const char next = p[i + 1];
        if (next == '%') {
            literal.push_back('%');
            ++i;
            continue;
        }
        const Field f = field_for(next);
        if (f == Field::Literal) {
            // Unknown token: keep both characters as text.
            literal.push_back('%');
            literal.push_back(next);
            ++i;
            continue;
        }
        flush_literal();
        spec->tokens.push_back({f, std::string()});
        if (f == Field::Time || f == Field::Elapsed)
            spec->needs_time = true;
        ++i;
    }
    flush_literal();
    return spec;
}

std::size_t build_line(const FormatSpec& fs, const Record& rec, bool colored, char* out,
                       std::size_t cap) {
    std::size_t pos   = 0;
    bool        trunc = false;

    // Copy n bytes, but never past cap - 1 (one byte is kept for the newline).
    auto put = [&](const char* s, std::size_t n) {
        if (trunc)
            return;
        if (pos + n > cap - 1) {
            n     = cap - 1 - pos;
            trunc = true;
        }
        std::memcpy(out + pos, s, n);
        pos += n;
    };
    auto put_str = [&](const char* s) { put(s, std::strlen(s)); };

    char tmp[64];
    for (const Token& t : fs.tokens) {
        switch (t.field) {
            case Field::Literal:
                put(t.literal.data(), t.literal.size());
                break;
            case Field::Level:
                if (colored) {
                    const char* col = color_for(rec.level->value);
                    if (col[0] != '\0') {
                        put_str(col);
                        put_str(rec.level->name);
                        put_str(kReset);
                        break;
                    }
                }
                put_str(rec.level->name);
                break;
            case Field::Func:
                put_str(rec.loc.func);
                break;
            case Field::Msg:
                put(rec.msg, rec.msg_len);
                break;
            case Field::Module:
                put_str(rec.module);
                break;
            case Field::Time: {
                const std::time_t secs = rec.now_epoch_ns / 1000000000;
                const int         ms =
                    static_cast<int>((rec.now_epoch_ns % 1000000000) / 1000000);
                std::tm tmv;
                localtime_r(&secs, &tmv);
                const int n = std::snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d.%03d",
                                            tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
                if (n > 0)
                    put(tmp, static_cast<std::size_t>(n));
                break;
            }
            case Field::Elapsed: {
                std::int64_t el = rec.now_epoch_ns - fs.start_epoch_ns;
                if (el < 0)
                    el = 0;
                const double sec = static_cast<double>(el) / 1e9;
                const int    n   = std::snprintf(tmp, sizeof(tmp), "%.3f", sec);
                if (n > 0)
                    put(tmp, static_cast<std::size_t>(n));
                break;
            }
            case Field::Pid: {
                const int n = std::snprintf(tmp, sizeof(tmp), "%d", rec.pid);
                if (n > 0)
                    put(tmp, static_cast<std::size_t>(n));
                break;
            }
            case Field::Tid: {
                const int n = std::snprintf(tmp, sizeof(tmp), "%ld", rec.tid);
                if (n > 0)
                    put(tmp, static_cast<std::size_t>(n));
                break;
            }
            case Field::File:
                put_str(rec.loc.file);
                break;
            case Field::Line: {
                const int n = std::snprintf(tmp, sizeof(tmp), "%d", rec.loc.line);
                if (n > 0)
                    put(tmp, static_cast<std::size_t>(n));
                break;
            }
        }
    }

    // A truncated line ends with "..." so a reader can see it was cut.
    if (trunc) {
        std::size_t marker = pos > cap - 4 ? cap - 4 : pos;
        std::memcpy(out + marker, "...", 3);
        pos = marker + 3;
    }
    out[pos++] = '\n';
    return pos;
}

}  // namespace detail
}  // namespace slog
