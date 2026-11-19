#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace telegram_mcpp {

enum class SenderType { user, chat };

struct Sender {
    SenderType type = SenderType::user;
    std::int64_t id = 0;
    bool operator==(const Sender& other) const { return type == other.type && id == other.id; }
};

struct Reaction {
    std::string type = "unsupported";
    std::string emoji;
    std::int64_t custom_emoji_id = 0;
    int total_count = 0;
    bool is_chosen = false;
};

struct MessageReactions {
    std::vector<Reaction> items;
    bool are_tags = false;
    bool truncated = false;
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
    std::optional<MessageReactions> reactions = {};
};

}
