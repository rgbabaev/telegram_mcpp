#include "telegram_mcpp/config.hpp"
#include "telegram_mcpp/private_storage.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace telegram_mcpp {
namespace {

std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == value.npos) return {};
    const auto last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}

using Fields = std::map<std::string, std::string>;

Fields parse_fields(const std::string& contents) {
    Fields fields;
    std::istringstream input(contents);
    std::string line;
    const std::set<std::string> known{
        "api_id", "api_hash", "state_directory", "allowed_chat_ids",
        "page_size", "request_timeout_seconds", "concurrent_requests"};
    while (std::getline(input, line)) {
        const auto value = trim(line);
        if (value.empty()) continue;
        const auto separator = value.find('=');
        if (separator == value.npos) throw std::runtime_error("Expected configuration key=value");
        const std::string key(trim(value.substr(0, separator)));
        if (known.count(key) == 0) throw std::runtime_error("Unknown configuration key");
        if (!fields.emplace(key, trim(value.substr(separator + 1))).second) {
            throw std::runtime_error("Duplicate configuration key");
        }
    }
    for (const auto& key : {"api_id", "api_hash", "state_directory", "allowed_chat_ids"}) {
        if (fields.count(key) == 0) throw std::runtime_error("Missing required configuration key");
    }
    return fields;
}

template <typename Integer>
Integer parse_integer(std::string_view value) {
    if (value.empty()) throw std::runtime_error("Invalid configuration integer");
    Integer result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::runtime_error("Invalid configuration integer");
    }
    return result;
}

int read_limit(const Fields& fields, const std::string& key, int fallback, int maximum) {
    const auto entry = fields.find(key);
    const int result = entry == fields.end() ? fallback : parse_integer<int>(entry->second);
    if (result < 1 || result > maximum) throw std::runtime_error("Configuration limit out of range");
    return result;
}

std::set<std::int64_t> parse_chat_ids(std::string_view value) {
    std::set<std::int64_t> ids;
    if (value.empty()) return ids;
    for (;;) {
        const auto separator = value.find(',');
        const auto id = parse_integer<std::int64_t>(trim(value.substr(0, separator)));
        if (id == 0 || !ids.insert(id).second) throw std::runtime_error("Invalid or duplicate chat ID");
        if (separator == value.npos) return ids;
        value.remove_prefix(separator + 1);
    }
}

}

void initialize_config(const std::filesystem::path& path) {
    create_private_file(path,
        "api_id=0\n"
        "api_hash=\n"
        "state_directory=state\n"
        "allowed_chat_ids=\n"
        "page_size=20\n"
        "request_timeout_seconds=30\n"
        "concurrent_requests=4\n");
}

Config load_config(const std::filesystem::path& path) {
    const auto fields = parse_fields(read_private_file(path));
    Config config{};
    config.api_id = parse_integer<std::int32_t>(fields.at("api_id"));
    if (config.api_id <= 0) throw std::runtime_error("api_id must be a positive 32-bit integer");
    config.api_hash = fields.at("api_hash");
    if (config.api_hash.size() != 32 ||
        !std::all_of(config.api_hash.begin(), config.api_hash.end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f') ||
                   (character >= 'A' && character <= 'F');
        })) {
        throw std::runtime_error("api_hash must contain 32 hexadecimal characters");
    }
    const auto& state = fields.at("state_directory");
    if (state.empty() || state.find('\0') != state.npos) {
        throw std::runtime_error("Invalid state directory");
    }
    const auto parent = std::filesystem::absolute(path).parent_path();
    config.state_directory = (parent / state).lexically_normal();
    config.allowed_chat_ids = parse_chat_ids(fields.at("allowed_chat_ids"));
    config.limits.page_size = read_limit(fields, "page_size", 20, Limits::maximum_page_size);
    config.limits.request_timeout = std::chrono::seconds(
        read_limit(fields, "request_timeout_seconds", 30, 30));
    config.limits.concurrent_requests = read_limit(fields, "concurrent_requests", 4, 4);
    return config;
}

void prepare_state_directory(const Config& config) {
    ensure_private_directory(config.state_directory);
}

}
