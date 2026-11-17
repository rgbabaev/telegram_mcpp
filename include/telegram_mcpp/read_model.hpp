#pragma once

#include <cstdint>
#include <string>

namespace telegram_mcpp {

struct MessageRecord {
    std::int64_t chat_id = 0;
    std::int64_t id = 0;
    std::int32_t date = 0;
    std::string text;
    std::string content_type = "unsupported";
    bool text_truncated = false;
};

}
