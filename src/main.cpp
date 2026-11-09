#include "telegram_mcpp/config.hpp"
#include "telegram_mcpp/logger.hpp"
#ifdef TELEGRAM_MCPP_WITH_TDLIB
#include "telegram_mcpp/tdlib.hpp"
#endif

#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    telegram_mcpp::Logger logger(std::cerr);
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cerr << "Usage: telegram-mcpp config <init|check> PATH\n"
                         "       telegram-mcpp <--help|--version>\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cerr << "telegram-mcpp 0.1.0\n";
#ifdef TELEGRAM_MCPP_WITH_TDLIB
            std::cerr << "TDLib " << telegram_mcpp::tdlib_version() << '\n';
#else
            std::cerr << "TDLib disabled (offline bootstrap build)\n";
#endif
            return 0;
        }
        if (argc != 4 || std::string_view(argv[1]) != "config") {
            std::cerr << "Invalid command; use --help\n";
            return 2;
        }
        const std::string_view action(argv[2]);
        if (action == "init") {
            telegram_mcpp::initialize_config(argv[3]);
            logger.write(telegram_mcpp::Event::config_created);
        } else if (action == "check") {
            const auto config = telegram_mcpp::load_config(argv[3]);
            telegram_mcpp::prepare_state_directory(config);
            logger.write(telegram_mcpp::Event::config_validated);
        } else {
            std::cerr << "Invalid configuration command; use --help\n";
            return 2;
        }
        return 0;
    } catch (const std::exception& error) {
        logger.write(telegram_mcpp::Event::command_failed);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
