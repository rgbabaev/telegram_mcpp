#include "telegram_mcpp/read_operations.hpp"
#include "telegram_mcpp/text.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>

namespace {

using namespace telegram_mcpp;
using namespace std::chrono_literals;

void require(bool condition) {
    if (!condition) throw std::runtime_error("Expectation failed");
}

template <typename Action>
void fails(ReadErrorCode code, Action action) {
    try { action(); }
    catch (const ReadError& error) { require(error.code() == code); return; }
    throw std::runtime_error("Expected read error");
}

class FakeTransport final : public AccountTransport {
public:
    using Handler = std::function<AccountResult(const AccountRequest&)>;
    explicit FakeTransport(Handler handler) : handler_(std::move(handler)) {
        AccountEvent event;
        event.authorization = AuthorizationUpdate{Authorization::ready, {}, 0, false};
        events_.push_back(event);
    }
    void send(std::uint64_t id, AccountRequest request) override {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(request);
        }
        auto result = handler_(request);
        push({id, std::move(result), {}});
    }
    std::optional<AccountEvent> receive(std::chrono::milliseconds timeout) override {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, timeout, [&] { return !events_.empty(); });
        if (events_.empty()) return {};
        auto result = std::move(events_.front());
        events_.pop_front();
        return result;
    }
    void push(AccountEvent event) {
        const std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(event));
        changed_.notify_all();
    }
    std::vector<AccountRequest> requests() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }
private:
    Handler handler_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<AccountRequest> requests_;
    std::deque<AccountEvent> events_;
};

struct Backend {
    std::map<std::int64_t, std::vector<MessageRecord>> history;
    std::string title = "private-title";
    std::optional<std::string> link;
    int batch_size = 100;
    bool exclusive = false;
    bool repeat_last_offset = false;
    AccountResult operator()(const AccountRequest& request) const {
        AccountResult result;
        if (request.command == AccountCommand::chat) {
            result.chat = ChatSummary{request.chat_id, title, "channel", false};
        } else if (request.command == AccountCommand::message_link) {
            result.link = link;
            if (!link) result.error = AccountErrorCode::message_unavailable;
        } else if (request.command == AccountCommand::message) {
            const auto found = history.find(request.chat_id);
            if (found != history.end()) {
                for (const auto& message : found->second) if (message.id == request.message_id) result.message = message;
            }
            if (!result.message) result.error = AccountErrorCode::message_unavailable;
        } else if (request.command == AccountCommand::search_messages) {
            require(request.limit > 0 && request.limit <= 100 && request.secret == "private-query");
            const auto found = history.find(request.chat_id);
            if (found == history.end()) return result;
            for (const auto& message : found->second) {
                if (request.message_id && (message.id > request.message_id || (exclusive && message.id == request.message_id))) continue;
                if (result.messages.size() == static_cast<std::size_t>(std::min(request.limit, batch_size))) {
                    result.next_from_message_id = message.id;
                    break;
                }
                result.messages.push_back(message);
            }
            if (repeat_last_offset && !result.messages.empty()) result.next_from_message_id = result.messages.back().id;
        } else throw std::runtime_error("Unexpected backend command");
        return result;
    }
};

Config configuration(std::set<std::int64_t> allowed = {1, 2}) {
    Config config;
    config.allowed_chat_ids = std::move(allowed);
    return config;
}

struct Fixture {
    std::ostringstream output;
    Logger logger{output};
    FakeTransport* fake;
    std::unique_ptr<AccountDispatcher> dispatcher;
    std::unique_ptr<ReadOperations> reads;
    Fixture(Config config, FakeTransport::Handler handler, CursorCodec::Clock clock = {}) {
        auto transport = std::make_unique<FakeTransport>(std::move(handler));
        fake = transport.get();
        dispatcher = std::make_unique<AccountDispatcher>(std::move(transport), logger, config.limits.concurrent_requests);
        AccountAccess access(*dispatcher, 1s, {});
        require(access.status().state == Authorization::ready);
        reads = std::make_unique<ReadOperations>(*dispatcher, config, std::move(clock));
    }
    ~Fixture() {
        dispatcher->shutdown();
        if (output.str().find("private-") != std::string::npos) std::abort();
    }
};

MessageRecord message(std::int64_t chat, std::int64_t id, std::int32_t date = 1704067200,
                      std::string text = "private-text") {
    return {chat, id, date, std::move(text), "text", false};
}

SearchMessagesInput search(int limit = 20) {
    SearchMessagesInput input;
    input.query = "private-query";
    input.limit = limit;
    return input;
}

void deny_by_default_and_validate() {
    Fixture fixture(configuration({}), Backend{});
    require(fixture.reads->list_chats({}).chats.empty());
    const auto page = fixture.reads->search_messages(search());
    require(page.status == SearchStatus::complete && page.messages.empty() && !page.next_cursor);
    fails(ReadErrorCode::denied_chat, [&] { fixture.reads->get_message("1", "2"); });
    auto input = search();
    input.chat_id = "1";
    fails(ReadErrorCode::denied_chat, [&] { fixture.reads->search_messages(input); });
    for (const auto* id : {"", "0", "01", "+1", " 1", "1x", "9223372036854775808"}) {
        fails(ReadErrorCode::invalid_input, [&] { fixture.reads->get_message(id, "2"); });
    }
    for (const auto* query : {"", "  \t\r\n", "\xff"}) {
        input = search(); input.query = query;
        fails(ReadErrorCode::invalid_input, [&] { fixture.reads->search_messages(input); });
    }
    input = search(); input.query = std::string("a\0b", 3);
    fails(ReadErrorCode::invalid_input, [&] { fixture.reads->search_messages(input); });
    input.query = std::string(4097, 'x');
    fails(ReadErrorCode::invalid_input, [&] { fixture.reads->search_messages(input); });
    for (const int limit : {0, -1, 101}) {
        fails(ReadErrorCode::invalid_input, [&] { fixture.reads->search_messages(search(limit)); });
        fails(ReadErrorCode::invalid_input, [&] { fixture.reads->list_chats({limit, {}}); });
    }
    require(fixture.fake->requests().empty());
}

void allowlisted_chat_pagination_and_precision() {
    Fixture fixture(configuration({INT64_MIN, -10, INT64_MAX}), Backend{});
    auto page = fixture.reads->list_chats({1, {}});
    require(page.chats.size() == 1 && page.chats[0].id == INT64_MIN && page.next_cursor);
    require(to_json(page).find("\"-9223372036854775808\"") != std::string::npos);
    page = fixture.reads->list_chats({1, page.next_cursor});
    require(page.chats[0].id == -10 && page.next_cursor);
    page = fixture.reads->list_chats({1, page.next_cursor});
    require(page.chats[0].id == INT64_MAX && !page.next_cursor);
    for (const auto& request : fixture.fake->requests()) {
        require(request.command == AccountCommand::chat);
        require(request.chat_id == INT64_MIN || request.chat_id == -10 || request.chat_id == INT64_MAX);
    }
}

void opaque_cursors_bind_scope_and_expire() {
    std::int64_t now = 1000;
    Backend backend;
    backend.history[1] = {message(1, 10), message(1, 9)};
    Fixture fixture(configuration(), backend, [&] { return now; });
    const auto cursor = fixture.reads->list_chats({1, {}}).next_cursor;
    require(cursor && cursor->find("private-") == std::string::npos);
    fails(ReadErrorCode::invalid_cursor, [&] { fixture.reads->list_chats({2, cursor}); });
    fails(ReadErrorCode::invalid_cursor, [&] { fixture.reads->search_messages({"private-query", {}, {}, {}, 1, cursor}); });
    for (const auto& token : {std::string{}, std::string("invalid"), cursor->substr(1), *cursor + "0"}) {
        fails(ReadErrorCode::invalid_cursor, [&] { fixture.reads->list_chats({1, token}); });
    }
    auto tampered = *cursor;
    tampered[20] = tampered[20] == '0' ? '1' : '0';
    fails(ReadErrorCode::invalid_cursor, [&] { fixture.reads->list_chats({1, tampered}); });
    Fixture restarted(configuration(), backend, [&] { return now; });
    fails(ReadErrorCode::invalid_cursor, [&] { restarted.reads->list_chats({1, cursor}); });
    const auto search_cursor = fixture.reads->search_messages(search(1)).next_cursor;
    require(search_cursor.has_value());
    auto input = search(1); input.cursor = search_cursor; input.chat_id = "1";
    fails(ReadErrorCode::invalid_cursor, [&] { fixture.reads->search_messages(input); });
    input.chat_id.reset(); input.since = "2020-01-01T00:00:00Z";
    fails(ReadErrorCode::invalid_cursor, [&] { fixture.reads->search_messages(input); });
    input.since.reset(); input.query = "changed-query";
    fails(ReadErrorCode::invalid_cursor, [&] { fixture.reads->search_messages(input); });
    now += 900;
    fails(ReadErrorCode::invalid_cursor, [&] { fixture.reads->list_chats({1, cursor}); });
}

void per_chat_search_and_continuation() {
    Backend backend;
    backend.batch_size = 2;
    backend.history[-100] = {message(-100, INT64_MAX), message(-100, INT64_MAX - 1), message(-100, 7)};
    backend.history[2] = {message(2, 8), message(2, 6)};
    backend.history[999] = {message(999, 50)};
    auto config = configuration({-100, 2});
    config.limits.page_size = 1;
    Fixture fixture(config, backend);
    require(fixture.reads->list_chats({}).chats.size() == 1);
    auto input = search(1);
    std::vector<std::string> ids;
    for (int page_number = 0; page_number < 10; ++page_number) {
        const auto page = fixture.reads->search_messages(input);
        require(!page.error);
        for (const auto& result : page.messages) ids.push_back(result.message_id);
        if (!page.next_cursor) { require(page.status == SearchStatus::complete); break; }
        require(page.status == SearchStatus::partial);
        input.cursor = page.next_cursor;
    }
    require(ids == std::vector<std::string>{"9223372036854775807", "9223372036854775806", "7", "8", "6"});
    for (const auto& request : fixture.fake->requests()) require(request.chat_id == -100 || request.chat_id == 2);
    input = search(); input.chat_id = "2";
    const auto single = fixture.reads->search_messages(input);
    require(single.status == SearchStatus::complete && single.messages.size() == 2);
}

void tdlib_last_message_offsets() {
    Backend backend;
    backend.batch_size = 2;
    backend.exclusive = true;
    backend.repeat_last_offset = true;
    for (int index = 12; index > 0; --index) backend.history[1].push_back(message(1, index));
    Fixture fixture(configuration({1}), backend);
    auto input = search(3);
    std::vector<std::string> ids;
    for (int attempt = 0; attempt < 10; ++attempt) {
        const auto page = fixture.reads->search_messages(input);
        require(!page.error);
        for (const auto& result : page.messages) ids.push_back(result.message_id);
        if (!page.next_cursor) { require(page.status == SearchStatus::complete); break; }
        input.cursor = page.next_cursor;
    }
    require(ids.size() == 12);
    for (std::size_t index = 0; index < ids.size(); ++index) require(ids[index] == std::to_string(12 - index));
}

void dates_are_half_open_and_strict() {
    Backend backend;
    backend.history[1] = {message(1, 3, 1704067202), message(1, 2, 1704067201), message(1, 1, 1704067200)};
    Fixture fixture(configuration({1}), backend);
    auto input = search();
    input.since = "2024-01-01T00:00:00.000000001Z";
    input.until = "2024-01-01T00:00:02+00:00";
    auto page = fixture.reads->search_messages(input);
    require(page.status == SearchStatus::complete && page.messages.size() == 1 && page.messages[0].message_id == "2");
    require(page.messages[0].timestamp == "2024-01-01T00:00:01Z");
    input.since = "2024-01-01T00:00:01Z";
    input.until = input.since;
    page = fixture.reads->search_messages(input);
    require(page.status == SearchStatus::complete && page.messages.empty());
    input.until = "2023-12-31T23:59:59Z";
    fails(ReadErrorCode::invalid_input, [&] { fixture.reads->search_messages(input); });
    input.until.reset();
    for (const auto* invalid : {"2024-02-30T00:00:00Z", "2023-02-29T00:00:00Z", "2024-00-01T00:00:00Z",
                              "2024-01-01T24:00:00Z", "2024-01-01T00:60:00Z", "2024-01-01T00:00:60Z",
                              "2024-01-01T00:00:00+01:00", "2024-01-01", "2024-01-01T00:00:00.Z",
                              "2024-01-01T00:00:00.1234567890Z", "0000-01-01T00:00:00Z"}) {
        input.since = invalid;
        fails(ReadErrorCode::invalid_input, [&] { fixture.reads->search_messages(input); });
    }
}

void bounded_empty_search_is_partial() {
    Backend backend;
    for (int index = 1100; index > 0; --index) backend.history[1].push_back(message(1, index));
    Fixture fixture(configuration({1}), backend);
    auto input = search();
    input.since = "2030-01-01T00:00:00Z";
    const auto page = fixture.reads->search_messages(input);
    require(page.messages.empty() && page.status == SearchStatus::partial && page.next_cursor && !page.error);
    int searches = 0;
    for (const auto& request : fixture.fake->requests()) if (request.command == AccountCommand::search_messages) ++searches;
    require(searches == 10);
    input.cursor = page.next_cursor;
    const auto last = fixture.reads->search_messages(input);
    require(last.messages.empty() && last.status == SearchStatus::complete && !last.next_cursor);
}

MessagePage large_page() {
    Backend backend;
    backend.title = "\"\n\x1b" + std::string(600, 'x');
    for (int index = 100; index > 0; --index) backend.history[1].push_back(message(1, index, 1704067200, std::string(9000, '\1')));
    Fixture fixture(configuration({1}), backend);
    auto input = search(100);
    const auto page = fixture.reads->search_messages(input);
    require(page.response_limited && page.next_cursor && page.status == SearchStatus::partial);
    require(!page.messages.empty() && page.messages.size() < 100 && to_json(page).size() <= Limits::response_bytes - 4096);
    for (const auto& result : page.messages) require(result.text.size() == 8192 && result.text_truncated && result.chat_title_truncated);
    input.cursor = page.next_cursor;
    const auto next = fixture.reads->search_messages(input);
    require(std::stoll(next.messages.front().message_id) == std::stoll(page.messages.back().message_id) - 1);
    return page;
}

void utf8_text_and_links() {
    Backend backend;
    backend.history[1] = {message(1, INT64_MAX, 0, std::string(8191, 'a') + "€")};
    backend.link = "https://t.me/example/123";
    Fixture fixture(configuration({1}), backend);
    const auto result = fixture.reads->get_message("1", "9223372036854775807");
    require(result.text.size() == 8191 && result.text_truncated && result.link == backend.link);
    require(result.timestamp == "1970-01-01T00:00:00Z");
    require(to_json(result).find("\"message_id\":\"9223372036854775807\"") != std::string::npos);
    for (const auto* bad : {"http://t.me/a/1", "https://t.me.evil/a", "https://t.me/a/1\n", "https://t.me/a/1\""}) {
        backend.link = bad;
        Fixture invalid(configuration({1}), backend);
        require(!invalid.reads->get_message("1", "9223372036854775807").link);
    }
    backend.link.reset();
    backend.history[1][0].content_type = "unknown-service-type";
    backend.history[1][0].text = "a\xff\xe2\x82\xac";
    Fixture invalid(configuration({1}), backend);
    const auto normalized = invalid.reads->get_message("1", "9223372036854775807");
    require(normalized.content_type == "unsupported" && valid_utf8(normalized.text) && normalized.text_truncated);
    fails(ReadErrorCode::message_unavailable, [&] { invalid.reads->get_message("1", "5"); });
    for (const auto* id : {"-1", "0", "01", "+1", "9223372036854775808"}) {
        fails(ReadErrorCode::invalid_input, [&] { invalid.reads->get_message("1", id); });
    }
}

void failed_and_partial_searches() {
    Backend backend;
    backend.history[1] = {message(1, 5)};
    std::atomic<bool> offline{true};
    Fixture fixture(configuration(), [&](const auto& request) {
        if (request.chat_id == 2 && offline) {
            AccountResult result; result.error = AccountErrorCode::unavailable; return result;
        }
        return backend(request);
    });
    const auto page = fixture.reads->search_messages(search());
    require(page.status == SearchStatus::partial && page.error && page.error->code == ReadErrorCode::unavailable);
    require(page.messages.size() == 1 && page.next_cursor);
    offline = false;
    auto input = search(); input.cursor = page.next_cursor;
    const auto resumed = fixture.reads->search_messages(input);
    require(resumed.status == SearchStatus::complete && resumed.messages.empty());
    for (const auto error : {AccountErrorCode::history_unavailable, AccountErrorCode::unavailable, AccountErrorCode::timeout}) {
        Fixture failed(configuration(), [error](const auto&) { AccountResult result; result.error = error; return result; });
        const auto result = failed.reads->search_messages(search());
        require(result.status == SearchStatus::failed && result.error && result.messages.empty() && result.next_cursor);
        require(to_json(result).find("\"status\":\"failed\"") != std::string::npos);
    }
}

void rate_delay_prevents_requests() {
    Backend backend;
    std::atomic<int> attempts{0};
    Fixture fixture(configuration(), [&](const auto& request) {
        if (request.command == AccountCommand::search_messages) {
            ++attempts;
            AccountResult result; result.error = AccountErrorCode::rate_limited; result.retry_after = 42; return result;
        }
        return backend(request);
    });
    const auto page = fixture.reads->search_messages(search());
    require(page.status == SearchStatus::failed && page.error && page.error->retry_after == 42);
    const auto count = fixture.fake->requests().size();
    fails(ReadErrorCode::rate_limited, [&] { fixture.reads->list_chats({}); });
    auto input = search(); input.cursor = page.next_cursor;
    const auto retry = fixture.reads->search_messages(input);
    require(retry.status == SearchStatus::failed && retry.error && retry.error->retry_after > 0);
    require(fixture.fake->requests().size() == count && attempts == 1);
}

void cancelled_revoked_and_overloaded() {
    Backend backend;
    Fixture fixture(configuration(), backend);
    fails(ReadErrorCode::cancelled, [&] { fixture.reads->list_chats({}, [] { return true; }); });
    fails(ReadErrorCode::cancelled, [&] { fixture.reads->search_messages(search(), [] { return true; }); });
    require(fixture.fake->requests().empty());
    const auto revision = fixture.dispatcher->authorization().revision;
    AccountEvent event; event.authorization = AuthorizationUpdate{Authorization::phone, {}, 0, false};
    fixture.fake->push(event);
    fixture.dispatcher->wait_authorization(revision, 1s, {});
    fails(ReadErrorCode::unauthenticated, [&] { fixture.reads->search_messages(search()); });
    require(fixture.fake->requests().empty());
    auto config = configuration(); config.limits.concurrent_requests = 1;
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future();
    std::atomic<bool> cancel{false};
    Fixture busy(config, [&](const auto& request) {
        entered.set_value(); released.wait(); return backend(request);
    });
    auto first = std::async(std::launch::async, [&] {
        fails(ReadErrorCode::cancelled, [&] { busy.reads->list_chats({}, [&] { return cancel.load(); }); });
    });
    require(entered.get_future().wait_for(1s) == std::future_status::ready);
    fails(ReadErrorCode::overloaded, [&] { busy.reads->search_messages(search()); });
    cancel = true;
    first.get();
    release.set_value();
}

void whole_operation_timeout() {
    Backend backend;
    backend.batch_size = 1;
    for (int index = 100; index > 0; --index) backend.history[1].push_back(message(1, index));
    auto config = configuration({1}); config.limits.request_timeout = 1s;
    Fixture fixture(config, [&](const auto& request) {
        std::this_thread::sleep_for(150ms);
        return backend(request);
    });
    auto input = search(); input.since = "2030-01-01T00:00:00Z";
    const auto started = std::chrono::steady_clock::now();
    const auto page = fixture.reads->search_messages(input);
    require(page.status == SearchStatus::partial && page.error && page.error->code == ReadErrorCode::timeout);
    require(std::chrono::steady_clock::now() - started < 1500ms);
}

void unsafe_backend_results_fail_closed() {
    Backend backend;
    for (const int mode : {0, 1, 2, 3}) {
        Fixture fixture(configuration({1}), [&](const auto& request) {
            auto result = backend(request);
            if (request.command == AccountCommand::search_messages) {
                if (mode == 0) result.messages = {message(999, 10)};
                if (mode == 1) result.messages = {message(1, 5), message(1, 6)};
                if (mode == 2) result.messages.resize(101, message(1, 5));
                if (mode == 3) result.next_from_message_id = -1;
            }
            return result;
        });
        const auto page = fixture.reads->search_messages(search());
        require(page.status != SearchStatus::complete && page.error);
        for (const auto& request : fixture.fake->requests()) require(request.chat_id == 1);
    }
    Fixture mismatch(configuration({1}), [&](const auto& request) {
        auto result = backend(request);
        if (request.command == AccountCommand::message) result.message = message(999, 10);
        return result;
    });
    fails(ReadErrorCode::message_unavailable, [&] { mismatch.reads->get_message("1", "10"); });
    int batches = 0;
    Fixture stalled(configuration({1}), [&](const auto& request) {
        auto result = backend(request);
        if (request.command == AccountCommand::search_messages) {
            ++batches; result.next_from_message_id = 10;
        }
        return result;
    });
    const auto page = stalled.reads->search_messages(search());
    require(page.status == SearchStatus::partial && page.messages.empty() && page.error &&
            page.error->code == ReadErrorCode::history_unavailable && batches == 2);
}

void chat_output_budget() {
    std::set<std::int64_t> ids;
    for (int id = 1; id <= 100; ++id) ids.insert(id);
    Backend backend; backend.title = std::string(512, '\1');
    Fixture fixture(configuration(ids), backend);
    const auto first = fixture.reads->list_chats({100, {}});
    require(first.next_cursor && first.chats.size() < 100 && to_json(first).size() < Limits::response_bytes - 4096);
    const auto second = fixture.reads->list_chats({100, first.next_cursor});
    require(!second.next_cursor && first.chats.size() + second.chats.size() == 100);
    require(second.chats.front().id == first.chats.back().id + 1);
}

}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--json") {
        std::cout << to_json(large_page());
        return EXIT_SUCCESS;
    }
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"deny by default and validation", deny_by_default_and_validate},
        {"allowed chat pages and ID precision", allowlisted_chat_pagination_and_precision},
        {"cursor binding and expiry", opaque_cursors_bind_scope_and_expire},
        {"per-chat search pagination", per_chat_search_and_continuation},
        {"TDLib last-message offsets", tdlib_last_message_offsets},
        {"UTC half-open dates", dates_are_half_open_and_strict},
        {"bounded empty partial search", bounded_empty_search_is_partial},
        {"bounded response and continuation", [] { large_page(); }},
        {"UTF-8, links, unsupported and unavailable messages", utf8_text_and_links},
        {"failed and partial searches", failed_and_partial_searches},
        {"rate cooldown", rate_delay_prevents_requests},
        {"cancellation, revocation and overload", cancelled_revoked_and_overloaded},
        {"whole-operation timeout", whole_operation_timeout},
        {"backend validation", unsafe_backend_results_fail_closed},
        {"chat output budget", chat_output_budget}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); }
        catch (const std::exception& error) { std::cerr << name << ": " << error.what() << '\n'; ++failures; }
    }
    std::cerr << tests.size() << " read cases, " << failures << " failures\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
