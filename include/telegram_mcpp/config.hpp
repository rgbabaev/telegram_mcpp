#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>

namespace telegram_mcpp {

struct Limits {
    int page_size = 20;
    std::chrono::seconds request_timeout{30};
    int concurrent_requests = 4;
    static constexpr int maximum_page_size = 100;
    static constexpr std::size_t response_bytes = 256 * 1024;
    static constexpr std::size_t message_text_bytes = 8 * 1024;
};

struct Config {
    std::int32_t api_id = 0;
    std::string api_hash;
    std::filesystem::path state_directory;
    std::set<std::int64_t> allowed_chat_ids;
    Limits limits;
};

void initialize_config(const std::filesystem::path& path);
Config load_config(const std::filesystem::path& path);
void prepare_state_directory(const Config& config);

}
