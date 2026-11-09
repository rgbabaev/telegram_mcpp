#include "telegram_mcpp/config.hpp"
#include "telegram_mcpp/logger.hpp"
#include "telegram_mcpp/private_storage.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace telegram_mcpp;

void require(bool condition) {
    if (!condition) throw std::runtime_error("Expectation failed");
}

template <typename Operation>
void require_failure(Operation operation) {
    try { operation(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("Expected failure");
}

class Workspace {
public:
    Workspace() {
        char pattern[] = "/tmp/telegram-mcpp-tests-XXXXXX";
        const auto directory = ::mkdtemp(pattern);
        if (!directory) throw std::runtime_error("Cannot create test directory");
        path = directory;
    }
    ~Workspace() { std::error_code error; fs::remove_all(path, error); }
    fs::path path;
};

std::string valid_config(std::string ids = "") {
    return "api_id=12345\napi_hash=0123456789abcdef0123456789abcdef\n"
           "state_directory=state\nallowed_chat_ids=" + ids + "\n";
}

mode_t permissions(const fs::path& path) {
    struct stat status {};
    require(::stat(path.c_str(), &status) == 0);
    return status.st_mode & 07777;
}

void defaults_and_precision() {
    Workspace workspace;
    const auto path = workspace.path / "config";
    create_private_file(path, valid_config("-9223372036854775808,9223372036854775807"));
    const auto config = load_config(path);
    require(config.api_id == 12345);
    require(config.limits.page_size == 20);
    require(config.limits.concurrent_requests == 4);
    require(config.limits.request_timeout.count() == 30);
    require(config.allowed_chat_ids.size() == 2);
    require(config.allowed_chat_ids.count(INT64_MIN) == 1);
    require(config.allowed_chat_ids.count(INT64_MAX) == 1);
    require(config.state_directory == workspace.path / "state");
    prepare_state_directory(config);
    require(permissions(config.state_directory) == 0700);
}

void empty_allowlist() {
    Workspace workspace;
    const auto path = workspace.path / "config";
    create_private_file(path, valid_config());
    require(load_config(path).allowed_chat_ids.empty());
}

void invalid_configuration() {
    const std::vector<std::string> cases{
        "", "api_id=1\n", valid_config() + "unknown=secret\n",
        valid_config() + "api_id=12345\n", valid_config() + "page_size=101\n",
        valid_config() + "page_size=0\n", valid_config() + "page_size=20x\n",
        valid_config() + "request_timeout_seconds=31\n",
        valid_config() + "concurrent_requests=5\n",
        valid_config("0"), valid_config("1,1"), valid_config("1,"),
        valid_config("9223372036854775808"), valid_config("1.5"), valid_config("+12"),
        "api_id=-1\napi_hash=x\nstate_directory=state\nallowed_chat_ids=\n",
        "api_id=2147483648\napi_hash=x\nstate_directory=state\nallowed_chat_ids=\n",
        "api_id=1\napi_hash=secret\nstate_directory=state\nallowed_chat_ids=\n",
        "api_id=1\napi_hash=0123456789abcdef0123456789abcdef\nstate_directory=\nallowed_chat_ids=\n"
    };
    for (const auto& contents : cases) {
        Workspace workspace;
        const auto path = workspace.path / "config";
        create_private_file(path, contents);
        require_failure([&] { load_config(path); });
    }
}

void initialization_is_private_and_exclusive() {
    Workspace workspace;
    const auto path = workspace.path / "nested" / "private" / "config";
    initialize_config(path);
    require(permissions(path) == 0600);
    require(permissions(path.parent_path()) == 0700);
    require_failure([&] { initialize_config(path); });
    require_failure([&] { load_config(path); });
}

void reject_unsafe_permissions() {
    Workspace workspace;
    const auto path = workspace.path / "config";
    create_private_file(path, valid_config());
    require(::chmod(path.c_str(), 0640) == 0);
    require_failure([&] { load_config(path); });
    require(::chmod(path.c_str(), 0600) == 0);
    require(::chmod(workspace.path.c_str(), 0755) == 0);
    require_failure([&] { load_config(path); });
}

void reject_links() {
    Workspace workspace;
    const auto path = workspace.path / "config";
    create_private_file(path, valid_config());
    fs::create_symlink(path, workspace.path / "link");
    require_failure([&] { load_config(workspace.path / "link"); });
    fs::create_directory_symlink(workspace.path, workspace.path / "directory-link");
    require_failure([&] { load_config(workspace.path / "directory-link" / "config"); });
    fs::create_hard_link(path, workspace.path / "hard-link");
    require_failure([&] { load_config(path); });
}

void reject_unsafe_state() {
    Workspace workspace;
    const auto path = workspace.path / "config";
    create_private_file(path, valid_config());
    const auto config = load_config(path);
    fs::create_directory(config.state_directory);
    require(::chmod(config.state_directory.c_str(), 0755) == 0);
    require_failure([&] { prepare_state_directory(config); });
    fs::remove(config.state_directory);
    fs::create_directory_symlink(workspace.path, config.state_directory);
    require_failure([&] { prepare_state_directory(config); });
}

void bounded_file_and_nonregular_file() {
    Workspace workspace;
    const auto path = workspace.path / "config";
    create_private_file(path, std::string(65537, 'x'));
    require_failure([&] { load_config(path); });
    const auto fifo = workspace.path / "fifo";
    require(::mkfifo(fifo.c_str(), 0600) == 0);
    require_failure([&] { load_config(fifo); });
}

void lower_limits_and_absolute_state() {
    Workspace workspace;
    const auto path = workspace.path / "config";
    create_private_file(path, "api_id=1\napi_hash=0123456789abcdef0123456789abcdef\n"
        "state_directory=" + workspace.path.string() + "/absolute\nallowed_chat_ids=\n"
        "page_size=1\nrequest_timeout_seconds=1\nconcurrent_requests=1\n");
    const auto config = load_config(path);
    require(config.limits.page_size == 1);
    require(config.limits.request_timeout.count() == 1);
    require(config.limits.concurrent_requests == 1);
    require(config.state_directory == workspace.path / "absolute");
}

void logs_contain_only_events() {
    std::ostringstream output;
    Logger logger(output);
    logger.write(Event::config_created);
    logger.write(Event::config_validated);
    logger.write(Event::command_failed);
    require(output.str() == "info config_created\ninfo config_validated\nerror command_failed\n");
}

}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"defaults and ID precision", defaults_and_precision},
        {"empty allowlist", empty_allowlist},
        {"strict configuration", invalid_configuration},
        {"private exclusive initialization", initialization_is_private_and_exclusive},
        {"unsafe permissions", reject_unsafe_permissions},
        {"symlinks and hard links", reject_links},
        {"unsafe state directory", reject_unsafe_state},
        {"bounded regular files", bounded_file_and_nonregular_file},
        {"configured limits and absolute state", lower_limits_and_absolute_state},
        {"event logging", logs_contain_only_events}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); }
        catch (const std::exception& error) {
            std::cerr << name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    std::cerr << tests.size() << " cases, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
