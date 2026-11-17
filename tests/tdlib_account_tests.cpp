#include "telegram_mcpp/account.hpp"
#include "telegram_mcpp/private_storage.hpp"
#include "telegram_mcpp/tdlib.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

int main() {
    char pattern[] = "/tmp/telegram-tdlib-tests-XXXXXX";
    const auto directory = ::mkdtemp(pattern);
    if (!directory) return 1;
    int result = 0;
    try {
        telegram_mcpp::Config config;
        config.api_id = 12345;
        config.api_hash = "0123456789abcdef0123456789abcdef";
        config.state_directory = directory;
        for (int restart = 0; restart < 2; ++restart) {
            telegram_mcpp::SessionLock lock(config.state_directory);
            std::ostringstream output;
            telegram_mcpp::Logger logger(output);
            telegram_mcpp::AccountDispatcher dispatcher(telegram_mcpp::make_tdlib_transport(config, logger), logger);
            telegram_mcpp::AccountAccess access(dispatcher, std::chrono::seconds(10), {});
            const auto state = access.status();
            if (state.state != telegram_mcpp::Authorization::phone) throw std::runtime_error("Unexpected authorization state");
            access.close();
            dispatcher.shutdown();
            if (output.str().find(config.api_hash) != std::string::npos) throw std::runtime_error("Credential leaked");
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(config.state_directory)) {
            struct stat status {};
            if (::lstat(entry.path().c_str(), &status) != 0 ||
                (status.st_mode & 0777) != (entry.is_directory() ? 0700 : 0600)) {
                throw std::runtime_error("Unsafe TDLib file permissions");
            }
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return result;
}
