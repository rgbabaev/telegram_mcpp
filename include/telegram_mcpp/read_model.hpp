#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace telegram_mcpp {

enum class SenderType { user, chat };

struct Sender {
    SenderType type = SenderType::user;
    std::int64_t id = 0;
    bool operator==(const Sender& other) const { return type == other.type && id == other.id; }
};

struct MessageRecord {
    std::int64_t chat_id = 0;
    std::int64_t id = 0;
    std::int32_t date = 0;
    std::string text;
    std::string content_type = "unsupported";
    bool text_truncated = false;
    std::optional<Sender> sender = {};
    bool is_outgoing = false;
};

}
