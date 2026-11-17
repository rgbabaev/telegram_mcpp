#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace telegram_mcpp {

struct CursorPosition {
    std::uint64_t chat_index = 0;
    std::int64_t from_message_id = 0;
    bool skip_first = false;
};

class CursorCodec {
public:
    using Clock = std::function<std::int64_t()>;
    explicit CursorCodec(Clock clock = {});
    std::string encode(const std::string& binding, CursorPosition position) const;
    CursorPosition decode(const std::string& binding, const std::string& cursor) const;
private:
    std::string signature(const std::string& binding, const std::string& payload) const;
    Clock clock_;
    std::array<unsigned char, 32> key_{};
};

}
