#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace telegram_mcpp {

void ensure_private_directory(const std::filesystem::path& path);
void create_private_file(const std::filesystem::path& path, std::string_view contents);
std::string read_private_file(const std::filesystem::path& path);

}
