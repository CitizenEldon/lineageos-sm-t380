#pragma once
#include <cctype>
#include <string>

namespace t380wifi {
inline bool Mac(std::string* value) {
    if (value->size() == 18 && value->back() == '\n') value->pop_back();
    if (value->size() != 17) return false;
    unsigned first = 0;
    bool nonzero = false;
    for (size_t i = 0; i < value->size(); ++i) {
        auto c = static_cast<unsigned char>((*value)[i]);
        if (i % 3 == 2) {
            if (c != ':') return false;
        } else {
            if (!std::isxdigit(c)) return false;
            (*value)[i] = static_cast<char>(std::tolower(c));
            unsigned nibble = std::isdigit(c) ? c - '0' : std::tolower(c) - 'a' + 10;
            if (i < 2) first = (first << 4) | nibble;
            nonzero |= nibble != 0;
        }
    }
    return nonzero && !(first & 1);  // Reject zero, broadcast, multicast.
}
}
