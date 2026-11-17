#include "telegram_mcpp/text.hpp"

namespace telegram_mcpp {
namespace {

std::size_t sequence_length(std::string_view text, std::size_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return 1;
    const std::size_t length = first >= 0xc2 && first <= 0xdf ? 2 :
        first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    if (!length || offset + length > text.size()) return 0;
    for (std::size_t index = 1; index < length; ++index) {
        if ((static_cast<unsigned char>(text[offset + index]) & 0xc0) != 0x80) return 0;
    }
    const auto second = static_cast<unsigned char>(text[offset + 1]);
    if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
        (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return 0;
    return length;
}

}

std::string bounded_utf8(std::string_view text, std::size_t maximum, bool& truncated) {
    std::string result;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto length = sequence_length(text, offset);
        const std::string_view part = length ? text.substr(offset, length) : std::string_view("\xef\xbf\xbd");
        if (result.size() + part.size() > maximum) { truncated = true; break; }
        result.append(part);
        if (!length) truncated = true;
        offset += length ? length : 1;
    }
    return result;
}

bool valid_utf8(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto length = sequence_length(text, offset);
        if (!length) return false;
        offset += length;
    }
    return true;
}

std::string json_string(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char character : text) {
        if (character == '"' || character == '\\') { result += '\\'; result += character; }
        else if (character < 0x20) {
            result += "\\u00";
            result += hex[character >> 4];
            result += hex[character & 15];
        } else result += character;
    }
    return result + '"';
}

}
