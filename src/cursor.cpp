#include "telegram_mcpp/cursor.hpp"
#include "telegram_mcpp/read_operations.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <chrono>
#include <limits>
#include <utility>

namespace telegram_mcpp {
namespace {

constexpr std::int64_t lifetime_seconds = 15 * 60;

void append_number(std::string& bytes, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) bytes += static_cast<char>(value >> shift);
}

std::uint64_t number(const std::string& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = offset; index < offset + 8; ++index) {
        value = (value << 8) | static_cast<unsigned char>(bytes[index]);
    }
    return value;
}

std::string hexadecimal(const std::string& bytes) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char byte : bytes) {
        result += alphabet[byte >> 4];
        result += alphabet[byte & 15];
    }
    return result;
}

std::string unhex(const std::string& text) {
    if (text.size() != 116) throw ReadError(ReadErrorCode::invalid_cursor);
    const auto digit = [](char value) -> unsigned char {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        throw ReadError(ReadErrorCode::invalid_cursor);
    };
    std::string result;
    for (std::size_t index = 0; index < text.size(); index += 2) {
        result += static_cast<char>((digit(text[index]) << 4) | digit(text[index + 1]));
    }
    return result;
}

}

CursorCodec::CursorCodec(Clock clock) : clock_(std::move(clock)) {
    if (!clock_) clock_ = [] {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    };
    if (RAND_bytes(key_.data(), key_.size()) != 1) throw ReadError(ReadErrorCode::unavailable);
}

std::string CursorCodec::signature(const std::string& binding, const std::string& payload) const {
    const auto input = binding + '\0' + payload;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (!HMAC(EVP_sha256(), key_.data(), key_.size(),
              reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest, &size)) {
        throw ReadError(ReadErrorCode::unavailable);
    }
    return std::string(reinterpret_cast<const char*>(digest), size);
}

std::string CursorCodec::encode(const std::string& binding, CursorPosition position) const {
    std::string payload(1, '\1');
    append_number(payload, clock_() + lifetime_seconds);
    append_number(payload, position.chat_index);
    append_number(payload, static_cast<std::uint64_t>(position.from_message_id));
    payload += static_cast<char>(position.skip_first);
    return hexadecimal(payload + signature(binding, payload));
}

CursorPosition CursorCodec::decode(const std::string& binding, const std::string& cursor) const {
    const auto bytes = unhex(cursor);
    const auto payload = bytes.substr(0, 26);
    const auto expected = signature(binding, payload);
    if (CRYPTO_memcmp(expected.data(), bytes.data() + 26, expected.size()) != 0) {
        throw ReadError(ReadErrorCode::invalid_cursor);
    }
    const auto expires = number(payload, 1);
    const auto now = static_cast<std::uint64_t>(clock_());
    const auto from = number(payload, 17);
    if (payload[0] != 1 || expires <= now || expires > now + lifetime_seconds ||
        from > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        (payload[25] != 0 && payload[25] != 1)) throw ReadError(ReadErrorCode::invalid_cursor);
    return {number(payload, 9), static_cast<std::int64_t>(from), payload[25] != 0};
}

}
