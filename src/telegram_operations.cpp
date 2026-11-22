#include "telegram_mcpp/telegram_operations.hpp"
#include "telegram_mcpp/text.hpp"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <limits>
#include <string_view>
#include <utility>

namespace telegram_mcpp {
namespace {

constexpr std::size_t result_budget = Limits::response_bytes - 4096;
constexpr std::size_t metadata_budget = 1024;
constexpr int maximum_search_batches = 10;
constexpr int maximum_chat_loads = 10;

std::int64_t identifier(const std::string& value, bool positive) {
    if (value.empty() || value.size() > 20) throw OperationError(OperationErrorCode::invalid_input);
    std::int64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        result == 0 || (positive && result < 0) || std::to_string(result) != value) {
        throw OperationError(OperationErrorCode::invalid_input);
    }
    return result;
}

bool valid_sender(const Sender& sender) {
    return (sender.type == SenderType::user && sender.id > 0) ||
        (sender.type == SenderType::chat && sender.id != 0);
}

void validate_draft_text(const std::string& text) {
    if (text.size() > Limits::draft_text_bytes || !valid_utf8(text) || text.find('\0') != std::string::npos) {
        throw OperationError(OperationErrorCode::invalid_input);
    }
    const auto characters = std::count_if(text.begin(), text.end(), [](unsigned char byte) { return (byte & 0xc0) != 0x80; });
    if (characters > static_cast<std::ptrdiff_t>(Limits::draft_text_characters) ||
        (!text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char byte) { return byte == ' ' || (byte >= 9 && byte <= 13); }))) {
        throw OperationError(OperationErrorCode::invalid_input);
    }
}

std::optional<Sender> sender_filter(const std::optional<SenderInput>& input) {
    if (!input) return {};
    if (input->type != "user" && input->type != "chat") throw OperationError(OperationErrorCode::invalid_input);
    return Sender{input->type == "user" ? SenderType::user : SenderType::chat,
                  identifier(input->id, input->type == "user")};
}

std::string search_query(const SearchMessagesInput& input) {
    if (!input.query) {
        if (!input.chat_id) throw OperationError(OperationErrorCode::invalid_input);
        return {};
    }
    const auto& query = *input.query;
    if (query.empty() || query.size() > 4096 || !valid_utf8(query) || query.find('\0') != std::string::npos ||
        std::all_of(query.begin(), query.end(), [](unsigned char c) { return c == ' ' || (c >= 9 && c <= 13); })) {
        throw OperationError(OperationErrorCode::invalid_input);
    }
    return query;
}

std::optional<MessageReactions> bounded_reactions(std::optional<MessageReactions> reactions) {
    if (!reactions) return {};
    if (reactions->items.size() > Limits::maximum_reaction_types) {
        reactions->items.resize(Limits::maximum_reaction_types);
        reactions->truncated = true;
    }
    for (auto& reaction : reactions->items) {
        if (reaction.total_count < 0 || (reaction.type == "custom_emoji" && !reaction.custom_emoji_id)) {
            throw OperationError(OperationErrorCode::unavailable);
        }
        if (reaction.type == "emoji") {
            reaction.emoji = bounded_utf8(reaction.emoji, Limits::reaction_emoji_bytes, reactions->truncated);
        } else if (reaction.type != "custom_emoji" && reaction.type != "paid") reaction.type = "unsupported";
    }
    return reactions;
}

struct UtcTime {
    std::int64_t seconds;
    int nanoseconds = 0;
    bool operator<(const UtcTime& other) const {
        return seconds < other.seconds || (seconds == other.seconds && nanoseconds < other.nanoseconds);
    }
};

UtcTime timestamp(const std::string& value) {
    const auto invalid = [] { throw OperationError(OperationErrorCode::invalid_input); };
    if (value.size() < 20 || value.size() > 35 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':') invalid();
    const auto component = [&](std::size_t start, std::size_t length) {
        int number = 0;
        for (std::size_t index = start; index < start + length; ++index) {
            if (value[index] < '0' || value[index] > '9') invalid();
            number = number * 10 + value[index] - '0';
        }
        return number;
    };
    std::tm date{};
    date.tm_year = component(0, 4) - 1900;
    date.tm_mon = component(5, 2) - 1;
    date.tm_mday = component(8, 2);
    date.tm_hour = component(11, 2);
    date.tm_min = component(14, 2);
    date.tm_sec = component(17, 2);
    if (date.tm_year < -1899 || date.tm_sec > 59) invalid();
    const auto original = date;
    const auto seconds = ::timegm(&date);
    if (date.tm_year != original.tm_year || date.tm_mon != original.tm_mon ||
        date.tm_mday != original.tm_mday || date.tm_hour != original.tm_hour ||
        date.tm_min != original.tm_min || date.tm_sec != original.tm_sec) invalid();
    std::size_t offset = 19;
    int nanos = 0;
    if (value[offset] == '.') {
        const auto start = ++offset;
        while (offset < value.size() && value[offset] >= '0' && value[offset] <= '9') ++offset;
        const auto digits = offset - start;
        if (!digits || digits > 9) invalid();
        nanos = component(start, digits);
        for (std::size_t index = digits; index < 9; ++index) nanos *= 10;
    }
    if (value.substr(offset) != "Z" && value.substr(offset) != "+00:00") invalid();
    return {seconds, nanos};
}

std::string timestamp(std::int32_t seconds) {
    const std::time_t value = seconds;
    std::tm date{};
    char formatted[21];
    if (!::gmtime_r(&value, &date) || !std::strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%SZ", &date)) {
        throw OperationError(OperationErrorCode::unavailable);
    }
    return formatted;
}

OperationError translated(const AccountError& error) {
    switch (error.code()) {
    case AccountErrorCode::unauthenticated:
    case AccountErrorCode::revoked: return OperationError(OperationErrorCode::unauthenticated);
    case AccountErrorCode::rate_limited: return OperationError(OperationErrorCode::rate_limited, error.retry_after());
    case AccountErrorCode::timeout: return OperationError(OperationErrorCode::timeout);
    case AccountErrorCode::cancelled: return OperationError(OperationErrorCode::cancelled);
    case AccountErrorCode::overloaded: return OperationError(OperationErrorCode::overloaded);
    case AccountErrorCode::message_unavailable: return OperationError(OperationErrorCode::message_unavailable);
    case AccountErrorCode::history_unavailable: return OperationError(OperationErrorCode::history_unavailable);
    case AccountErrorCode::draft_unavailable: return OperationError(OperationErrorCode::draft_unavailable);
    default: return OperationError(OperationErrorCode::unavailable);
    }
}

bool valid_link(const std::string& link) {
    constexpr std::string_view prefix = "https://t.me/";
    return link.size() > prefix.size() && link.size() <= 2048 &&
        link.compare(0, prefix.size(), prefix) == 0 &&
        std::all_of(link.begin() + prefix.size(), link.end(), [](unsigned char value) {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                (value >= '0' && value <= '9') || std::string_view("/_-?&=+.#%").find(value) != std::string_view::npos;
        });
}

void validate_batch(const AccountResult& batch, std::int64_t chat_id, std::int64_t from,
                    const std::optional<Sender>& sender) {
    if (batch.messages.size() > 100 || batch.next_from_message_id < 0) throw OperationError(OperationErrorCode::unavailable);
    auto previous = from;
    for (std::size_t index = 0; index < batch.messages.size(); ++index) {
        const auto& record = batch.messages[index];
        if (record.chat_id != chat_id || record.id <= 0 || record.date < 0 ||
            (previous && record.id > previous) || (index && record.id == previous) ||
            (record.sender && !valid_sender(*record.sender)) ||
            (sender && (!record.sender || !(*record.sender == *sender)))) {
            throw OperationError(OperationErrorCode::unavailable);
        }
        previous = record.id;
    }
    if (batch.next_from_message_id && ((from && batch.next_from_message_id >= from) ||
        (!batch.messages.empty() && batch.next_from_message_id > previous))) {
        throw OperationError(OperationErrorCode::history_unavailable);
    }
}

void advance(CursorPosition& position, const AccountResult& batch) {
    if (!batch.next_from_message_id) {
        ++position.chat_index;
        position.from_message_id = 0;
        position.skip_first = false;
    } else {
        position.from_message_id = batch.next_from_message_id;
        position.skip_first = !batch.messages.empty() && batch.messages.back().id == batch.next_from_message_id;
    }
}

std::string content_type(const std::string& type) {
    for (const auto* known : {"text", "photo", "video", "animation", "audio", "document",
                              "voice_note", "video_note", "sticker", "poll", "location", "contact"}) {
        if (type == known) return type;
    }
    return "unsupported";
}

}

const char* operation_error_name(OperationErrorCode code) {
    switch (code) {
    case OperationErrorCode::invalid_input: return "invalid_input";
    case OperationErrorCode::invalid_cursor: return "invalid_cursor";
    case OperationErrorCode::unauthenticated: return "unauthenticated";
    case OperationErrorCode::denied_chat: return "denied_chat";
    case OperationErrorCode::message_unavailable: return "message_unavailable";
    case OperationErrorCode::history_unavailable: return "history_unavailable";
    case OperationErrorCode::draft_unavailable: return "draft_unavailable";
    case OperationErrorCode::rate_limited: return "rate_limited";
    case OperationErrorCode::timeout: return "timeout";
    case OperationErrorCode::cancelled: return "cancelled";
    case OperationErrorCode::overloaded: return "overloaded";
    case OperationErrorCode::unavailable: return "unavailable";
    }
    return "unavailable";
}

OperationError::OperationError(OperationErrorCode code, int retry_after)
    : std::runtime_error(operation_error_name(code)), failure_{code, retry_after} {}

class TelegramOperations::Operation {
public:
    Operation(TelegramOperations& owner, const Cancelled& cancelled)
        : owner_(owner), cancelled_(cancelled), deadline_(std::chrono::steady_clock::now() + owner.config_.limits.request_timeout) {
        if (owner_.active_.fetch_add(1) >= owner_.config_.limits.concurrent_requests) {
            owner_.active_.fetch_sub(1);
            throw OperationError(OperationErrorCode::overloaded);
        }
    }
    ~Operation() { owner_.active_.fetch_sub(1); }
    std::chrono::milliseconds remaining() const {
        if (cancelled_ && cancelled_()) throw OperationError(OperationErrorCode::cancelled);
        const auto state = owner_.dispatcher_.authorization();
        if (state.state != Authorization::ready) throw OperationError(OperationErrorCode::unauthenticated);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) throw OperationError(OperationErrorCode::timeout);
        return remaining;
    }
    const Cancelled& cancelled() const { return cancelled_; }
private:
    TelegramOperations& owner_;
    const Cancelled& cancelled_;
    std::chrono::steady_clock::time_point deadline_;
};

TelegramOperations::TelegramOperations(AccountDispatcher& dispatcher, const Config& config, CursorCodec::Clock clock)
    : dispatcher_(dispatcher), config_(config), allowed_(config.allowed_chat_ids.begin(), config.allowed_chat_ids.end()),
      cursors_(std::move(clock)) {
    if (config.limits.page_size < 1 || config.limits.page_size > 100 ||
        config.limits.request_timeout.count() < 1 || config.limits.request_timeout.count() > 30 ||
        config.limits.concurrent_requests < 1 || config.limits.concurrent_requests > 4 || config.allowed_chat_ids.count(0)) {
        throw OperationError(OperationErrorCode::invalid_input);
    }
}

int TelegramOperations::page_size(std::optional<int> limit) const {
    const int size = limit.value_or(config_.limits.page_size);
    if (size < 1 || size > Limits::maximum_page_size) throw OperationError(OperationErrorCode::invalid_input);
    return size;
}

void TelegramOperations::require_allowed(std::int64_t id) const {
    if (!allowed_.empty() && !config_.allowed_chat_ids.count(id)) throw OperationError(OperationErrorCode::denied_chat);
}

std::string TelegramOperations::binding(const std::string& operation, int limit, const std::string& filters) const {
    std::string result = operation + ':' + std::to_string(limit) + (allowed_.empty() ? ":all:" : ":selected:");
    for (const auto id : allowed_) result += std::to_string(id) + ',';
    return result + ':' + filters;
}

AccountResult TelegramOperations::call(AccountRequest request, Operation& operation) {
    if (request.command == AccountCommand::load_chats || request.command == AccountCommand::chat_ids) {
        if (!allowed_.empty()) throw OperationError(OperationErrorCode::denied_chat);
    } else require_allowed(request.chat_id);
    const auto timeout = operation.remaining();
    {
        const std::lock_guard<std::mutex> lock(retry_mutex_);
        const auto delay = retry_until_ - std::chrono::steady_clock::now();
        if (delay > std::chrono::steady_clock::duration::zero()) {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(delay).count() + 1;
            throw OperationError(OperationErrorCode::rate_limited, static_cast<int>(seconds));
        }
    }
    try {
        auto result = dispatcher_.call(std::move(request), timeout, operation.cancelled());
        operation.remaining();
        return result;
    } catch (const AccountError& error) {
        if (error.code() == AccountErrorCode::rate_limited) {
            const std::lock_guard<std::mutex> lock(retry_mutex_);
            retry_until_ = std::max(retry_until_, std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, error.retry_after())));
        }
        throw translated(error);
    }
}

std::optional<std::vector<std::int64_t>> TelegramOperations::accessible_chats(Operation& operation) {
    if (!allowed_.empty()) return allowed_;
    std::vector<std::int64_t> ids;
    int loads = 0;
    for (const bool archive : {false, true}) {
        AccountRequest request{AccountCommand::load_chats, {}};
        request.archive = archive;
        request.limit = 100;
        for (;;) {
            if (loads == maximum_chat_loads) return {};
            ++loads;
            if (call(request, operation).exhausted) break;
        }
        request.command = AccountCommand::chat_ids;
        request.limit = std::numeric_limits<int>::max();
        auto result = call(request, operation);
        if (std::find(result.chat_ids.begin(), result.chat_ids.end(), 0) != result.chat_ids.end()) {
            throw OperationError(OperationErrorCode::unavailable);
        }
        ids.insert(ids.end(), result.chat_ids.begin(), result.chat_ids.end());
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

ChatSummary TelegramOperations::chat(std::int64_t id, Operation& operation) {
    AccountRequest request{AccountCommand::chat, {}};
    request.chat_id = id;
    auto result = call(request, operation);
    if (!result.chat || result.chat->id != id) throw OperationError(OperationErrorCode::history_unavailable);
    auto chat = std::move(*result.chat);
    chat.title = bounded_utf8(chat.title, 512, chat.title_truncated);
    if (chat.type != "private" && chat.type != "basic_group" && chat.type != "supergroup" &&
        chat.type != "channel" && chat.type != "secret") chat.type = "unsupported";
    if (chat.type == "private") {
        if (!chat.peer_user_id || *chat.peer_user_id <= 0) throw OperationError(OperationErrorCode::unavailable);
    } else chat.peer_user_id.reset();
    return chat;
}

Message TelegramOperations::present(MessageRecord record, const ChatSummary& chat, Operation& operation) {
    if (record.chat_id != chat.id || record.id <= 0 || record.date < 0 ||
        (record.sender && !valid_sender(*record.sender))) throw OperationError(OperationErrorCode::unavailable);
    Message result;
    result.chat_id = std::to_string(record.chat_id);
    result.chat_title = chat.title;
    result.chat_title_truncated = chat.title_truncated;
    result.message_id = std::to_string(record.id);
    result.timestamp = timestamp(record.date);
    result.text_truncated = record.text_truncated;
    result.text = bounded_utf8(record.text, Limits::message_text_bytes, result.text_truncated);
    result.content_type = content_type(record.content_type);
    result.sender = record.sender;
    result.is_outgoing = record.is_outgoing;
    result.reactions = bounded_reactions(std::move(record.reactions));
    AccountRequest request{AccountCommand::message_link, {}};
    request.chat_id = record.chat_id;
    request.message_id = record.id;
    try {
        auto response = call(request, operation);
        if (response.link && valid_link(*response.link)) result.link = std::move(response.link);
    } catch (const OperationError& error) {
        if (error.code() != OperationErrorCode::message_unavailable) throw;
    }
    return result;
}

AllowedChatPage TelegramOperations::list_chats(const ListChatsInput& input, const Cancelled& cancelled) {
    const auto limit = page_size(input.limit);
    const auto filters = binding("list_chats", limit);
    const auto position = input.cursor ? cursors_.decode(filters, *input.cursor) : CursorPosition{};
    if (position.from_message_id || position.skip_first) {
        throw OperationError(OperationErrorCode::invalid_cursor);
    }
    Operation operation(*this, cancelled);
    operation.remaining();
    AllowedChatPage page;
    const auto scope = accessible_chats(operation);
    if (!scope) {
        page.next_cursor = cursors_.encode(filters, position);
        return page;
    }
    if (input.cursor && position.chat_index >= scope->size() && !(scope->empty() && position.chat_index == 0)) {
        throw OperationError(OperationErrorCode::invalid_cursor);
    }
    std::size_t index = position.chat_index;
    while (index < scope->size() && page.chats.size() < static_cast<std::size_t>(limit)) {
        page.chats.push_back(chat((*scope)[index], operation));
        if (to_json(page).size() + metadata_budget > result_budget) { page.chats.pop_back(); break; }
        ++index;
    }
    if (index < scope->size()) page.next_cursor = cursors_.encode(filters, {index, 0, false});
    operation.remaining();
    return page;
}

Message TelegramOperations::get_message(const std::string& chat_id, const std::string& message_id, const Cancelled& cancelled) {
    const auto id = identifier(chat_id, false);
    const auto message = identifier(message_id, true);
    require_allowed(id);
    Operation operation(*this, cancelled);
    const auto summary = chat(id, operation);
    AccountRequest request{AccountCommand::message, {}};
    request.chat_id = id;
    request.message_id = message;
    auto result = call(request, operation);
    if (!result.message || result.message->chat_id != id || result.message->id != message) {
        throw OperationError(OperationErrorCode::message_unavailable);
    }
    return present(std::move(*result.message), summary, operation);
}

DraftResult TelegramOperations::set_draft(const std::string& chat_id, const std::string& text, const Cancelled& cancelled) {
    const auto id = identifier(chat_id, false);
    require_allowed(id);
    validate_draft_text(text);
    Operation operation(*this, cancelled);
    const auto summary = chat(id, operation);
    if (summary.type == "secret" || summary.type == "unsupported") throw OperationError(OperationErrorCode::draft_unavailable);
    AccountRequest request{AccountCommand::set_draft, text};
    request.chat_id = id;
    call(std::move(request), operation);
    return {std::to_string(id), text.empty()};
}

MessagePage TelegramOperations::search_messages(const SearchMessagesInput& input, const Cancelled& cancelled) {
    const auto limit = page_size(input.limit);
    const auto query = search_query(input);
    const auto sender = sender_filter(input.sender);
    const auto since = input.since ? std::optional<UtcTime>(timestamp(*input.since)) : std::nullopt;
    const auto until = input.until ? std::optional<UtcTime>(timestamp(*input.until)) : std::nullopt;
    if (since && until && *until < *since) throw OperationError(OperationErrorCode::invalid_input);
    auto scope = allowed_;
    if (input.chat_id) {
        const auto id = identifier(*input.chat_id, false);
        require_allowed(id);
        scope = {id};
    }
    const auto filters = binding("search_messages", limit, json_string(query) + ':' +
        (input.chat_id ? *input.chat_id : "*") + ':' +
        (since ? std::to_string(since->seconds) + '.' + std::to_string(since->nanoseconds) : "*") + ':' +
        (until ? std::to_string(until->seconds) + '.' + std::to_string(until->nanoseconds) : "*") + ':' +
        (sender ? input.sender->type + ':' + std::to_string(sender->id) : "*"));
    auto position = input.cursor ? cursors_.decode(filters, *input.cursor) : CursorPosition{};
    if (position.skip_first && !position.from_message_id) {
        throw OperationError(OperationErrorCode::invalid_cursor);
    }
    Operation operation(*this, cancelled);
    operation.remaining();
    MessagePage page;
    if (!input.chat_id) {
        try {
            const auto discovered = accessible_chats(operation);
            if (!discovered) {
                page.status = SearchStatus::partial;
                page.next_cursor = cursors_.encode(filters, position);
                return page;
            }
            scope = *discovered;
        } catch (const OperationError& error) {
            if (error.code() == OperationErrorCode::cancelled || error.code() == OperationErrorCode::unauthenticated) throw;
            page.status = SearchStatus::failed;
            page.error = OperationFailure{error.code(), error.retry_after()};
            page.next_cursor = cursors_.encode(filters, position);
            return page;
        }
    }
    if (input.cursor && position.chat_index >= scope.size() && !(scope.empty() && position.chat_index == 0)) {
        throw OperationError(OperationErrorCode::invalid_cursor);
    }
    std::size_t bytes = metadata_budget;
    int batches = 0;
    bool progressed = false;
    while (position.chat_index < scope.size() && page.messages.size() < static_cast<std::size_t>(limit) &&
           batches < maximum_search_batches && !page.response_limited) {
        try {
            const auto summary = chat(scope[position.chat_index], operation);
            if (sender && summary.type == "secret") throw OperationError(OperationErrorCode::history_unavailable);
            AccountRequest request{AccountCommand::search_messages, query};
            request.chat_id = summary.id;
            request.message_id = position.from_message_id;
            request.limit = 100;
            request.sender = sender;
            const auto batch = call(request, operation);
            ++batches;
            validate_batch(batch, summary.id, request.message_id, sender);
            const bool skip_first = position.skip_first;
            bool stopped = false;
            for (std::size_t index = 0; index < batch.messages.size(); ++index) {
                const auto& record = batch.messages[index];
                if (skip_first && record.id == request.message_id) continue;
                const UtcTime date{record.date, 0};
                if ((!since || !(date < *since)) && (!until || date < *until)) {
                    auto message = present(record, summary, operation);
                    const auto size = to_json(message).size() + 1;
                    if (bytes + size > result_budget) { page.response_limited = true; stopped = true; break; }
                    bytes += size;
                    page.messages.push_back(std::move(message));
                }
                position.from_message_id = record.id;
                position.skip_first = true;
                progressed = true;
                if (page.messages.size() == static_cast<std::size_t>(limit) && index + 1 < batch.messages.size()) {
                    stopped = true;
                    break;
                }
            }
            if (stopped) break;
            advance(position, batch);
            progressed = true;
        } catch (const OperationError& error) {
            if (error.code() == OperationErrorCode::cancelled || error.code() == OperationErrorCode::unauthenticated) throw;
            page.error = OperationFailure{error.code(), error.retry_after()};
            page.status = progressed ? SearchStatus::partial : SearchStatus::failed;
            page.next_cursor = cursors_.encode(filters, position);
            return page;
        }
    }
    if (position.chat_index < scope.size()) {
        page.status = SearchStatus::partial;
        page.next_cursor = cursors_.encode(filters, position);
    }
    operation.remaining();
    return page;
}

}
