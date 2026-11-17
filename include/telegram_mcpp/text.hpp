#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace telegram_mcpp {

std::string bounded_utf8(std::string_view text, std::size_t maximum, bool& truncated);
bool valid_utf8(std::string_view text);
std::string json_string(std::string_view text);

}
