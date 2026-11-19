#pragma once

#include "telegram_mcpp/telegram_operations.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <initializer_list>
#include <stdexcept>

namespace telegram_mcpp::mcp {

using Json = nlohmann::json;
inline constexpr const char* protocol_version = "2025-06-18";
inline constexpr std::size_t maximum_input = 64 * 1024;

class ProtocolError : public std::runtime_error {
public:
    ProtocolError(int code, const char* message) : std::runtime_error(message), code(code) {}
    int code;
};

Json parse(const std::string& line);
bool valid_id(const Json& id);
void fields(const Json& object, std::initializer_list<const char*> allowed);
Json result(const Json& id, Json value);
Json error(const Json& id, int code, const char* message);
Json tool_error(const Json& id, OperationErrorCode code, int retry_after = 0);
Json tools();
void validate_call(const Json& params);
Json execute(TelegramOperations& operations, const Json& id, const Json& params, const Cancelled& cancelled);

}
