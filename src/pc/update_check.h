#pragma once

// The launcher's update check, the parts that need no network (tested in tests/test_update_check.cpp).

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace xport::pc {

// The numbers in a version text, in order: "v0.3.10" is {0, 3, 10}, compared as a vector.
inline std::vector<int> version_numbers(const std::string& text) {
    std::vector<int> numbers;
    for (size_t i = 0; i < text.size();) {
        if (std::isdigit(static_cast<unsigned char>(text[i]))) {
            size_t end = i;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
            numbers.push_back(std::atoi(text.substr(i, end - i).c_str()));
            i = end;
        } else {
            ++i;
        }
    }
    return numbers;
}

// A link from GitHub's answer is used only if it is the repository's own `https://github.com/<repository>/releases/
// <kind>...` page or download, in plain characters with no "..": a download's last part becomes a file name in
// %TEMP%.
inline bool release_link(const std::string& url, const char* repository, const char* kind) {
    const std::string prefix = std::string("https://github.com/") + repository + "/releases/" + kind;
    if (url.size() <= prefix.size() || url.compare(0, prefix.size(), prefix) != 0) return false;
    if (url.find("..") != std::string::npos || url.back() == '/') return false;
    for (size_t i = prefix.size(); i < url.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(url[i]);
        if (!std::isalnum(c) && !std::strchr("._-/", c)) return false;
    }
    return true;
}

// The text of a JSON string value (the first `"key":"..."`, as GitHub writes it), its escapes undone: \n, \", \\, \/,
// \t and \uXXXX (UTF-8; a surrogate pair as one character). Empty when the key is missing.
inline std::string json_text(const std::string& json, const char* key) {
    const std::string marker = std::string("\"") + key + "\":\"";
    const size_t at = json.find(marker);
    if (at == std::string::npos) return {};
    std::string text;
    const auto put = [&text](unsigned code) {
        if (code < 0x80) {
            text += static_cast<char>(code);
        } else if (code < 0x800) {
            text += static_cast<char>(0xC0 | code >> 6), text += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            text += static_cast<char>(0xE0 | code >> 12), text += static_cast<char>(0x80 | (code >> 6 & 0x3F));
            text += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            text += static_cast<char>(0xF0 | code >> 18), text += static_cast<char>(0x80 | (code >> 12 & 0x3F));
            text += static_cast<char>(0x80 | (code >> 6 & 0x3F)), text += static_cast<char>(0x80 | (code & 0x3F));
        }
    };
    const auto hex4 = [&json](size_t from, unsigned& code) {
        if (from + 4 > json.size()) return false;
        code = 0;
        for (size_t i = from; i < from + 4; ++i) {
            const char c = json[i];
            const unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (digit == 16) return false;
            code = code << 4 | digit;
        }
        return true;
    };
    for (size_t i = at + marker.size(); i < json.size() && json[i] != '"'; ++i) {
        if (json[i] != '\\' || i + 1 == json.size()) {
            text += json[i];
            continue;
        }
        const char escape = json[++i];
        unsigned code = 0, low = 0;
        switch (escape) {
            case 'n': text += '\n'; break;
            case 't': text += '\t'; break;
            case 'r': break;
            case 'u':
                if (!hex4(i + 1, code)) return text;
                i += 4;
                if (code >= 0xD800 && code < 0xDC00 && i + 6 < json.size() && json[i + 1] == '\\' && json[i + 2] == 'u' &&
                    hex4(i + 3, low) && low >= 0xDC00 && low < 0xE000) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    i += 6;
                }
                put(code);
                break;
            default: text += escape; break;  // \" \\ \/
        }
    }
    return text;
}

// The points of a release's notes, for the launcher's notice: the top-level list items ("- " or "* " at the start of a
// line, with the lines that continue them), without Markdown (bold, code, a link's address) and cut after the first
// sentence; at most `count`.
inline std::vector<std::string> release_points(const std::string& notes, size_t count) {
    std::vector<std::string> points;
    std::string point;
    bool in_point = false;
    const auto finish = [&] {
        if (!in_point) return;
        in_point = false;
        std::string plain;
        for (size_t i = 0; i < point.size(); ++i) {
            if (point[i] == '*' || point[i] == '`') continue;
            if (point[i] == ']' && i + 1 < point.size() && point[i + 1] == '(') {  // [text](address): the text
                const size_t close = point.find(')', i);
                if (close != std::string::npos) i = close;
                continue;
            }
            if (point[i] == '[') continue;
            plain += point[i];
        }
        if (const size_t at = plain.find(". "); at != std::string::npos) plain.resize(at + 1);
        while (!plain.empty() && plain.back() == ' ') plain.pop_back();
        if (!plain.empty() && points.size() < count) points.push_back(plain);
    };
    for (size_t start = 0; start < notes.size();) {
        size_t end = notes.find('\n', start);
        if (end == std::string::npos) end = notes.size();
        std::string line = notes.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ') {
            finish();
            point = line.substr(2);
            in_point = true;
        } else if (in_point && line.size() > 1 && line[0] == ' ' && line.find_first_not_of(' ') != std::string::npos &&
                   line[line.find_first_not_of(' ')] != '-' && line[line.find_first_not_of(' ')] != '*') {
            point += ' ' + line.substr(line.find_first_not_of(' '));
        } else {
            finish();
        }
    }
    finish();
    return points;
}

}  // namespace xport::pc
