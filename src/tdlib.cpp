#include "telegram_mcpp/tdlib.hpp"
#include "telegram_mcpp/private_storage.hpp"
#include "telegram_mcpp/tdlib_read.hpp"

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace telegram_mcpp {

std::string tdlib_version() {
    auto logging = td::ClientManager::execute(td::td_api::make_object<td::td_api::setLogStream>(
        td::td_api::make_object<td::td_api::logStreamEmpty>()));
    if (logging->get_id() != td::td_api::ok::ID) throw std::runtime_error("Cannot disable TDLib logs");
    auto result = td::ClientManager::execute(td::td_api::make_object<td::td_api::getOption>("version"));
    if (result->get_id() != td::td_api::optionValueString::ID) {
        throw std::runtime_error("Cannot determine TDLib version");
    }
    return static_cast<const td::td_api::optionValueString&>(*result).value_;
}

namespace {

namespace api = td::td_api;

AuthorizationUpdate authorization(const api::AuthorizationState& state) {
    AuthorizationUpdate update;
    switch (state.get_id()) {
    case api::authorizationStateWaitTdlibParameters::ID: update.state = Authorization::parameters; break;
    case api::authorizationStateWaitPhoneNumber::ID: update.state = Authorization::phone; break;
    case api::authorizationStateWaitOtherDeviceConfirmation::ID:
        update.state = Authorization::qr;
        update.qr_link = static_cast<const api::authorizationStateWaitOtherDeviceConfirmation&>(state).link_;
        break;
    case api::authorizationStateWaitEmailAddress::ID: update.state = Authorization::email; break;
    case api::authorizationStateWaitEmailCode::ID: update.state = Authorization::email_code; break;
    case api::authorizationStateWaitCode::ID: update.state = Authorization::code; break;
    case api::authorizationStateWaitPassword::ID: update.state = Authorization::password; break;
    case api::authorizationStateWaitRegistration::ID: update.state = Authorization::registration; break;
    case api::authorizationStateReady::ID: update.state = Authorization::ready; break;
    case api::authorizationStateLoggingOut::ID: update.state = Authorization::logging_out; break;
    case api::authorizationStateClosing::ID: update.state = Authorization::closing; break;
    case api::authorizationStateClosed::ID: update.state = Authorization::closed; break;
    default: update.state = Authorization::unsupported; break;
    }
    return update;
}

class TdlibTransport final : public AccountTransport {
public:
    TdlibTransport(const Config& config, Logger& logger)
        : config_(config), logger_(logger),
          database_key_(read_private_file(config.state_directory / ".database.key")) {
        tdlib_version();
        manager_ = std::make_unique<td::ClientManager>();
        client_id_ = manager_->create_client_id();
        manager_->send(client_id_, std::numeric_limits<std::uint64_t>::max(), api::make_object<api::getOption>("version"));
    }

    void send(std::uint64_t id, AccountRequest request) override {
        requests_.emplace(id, request.command);
        manager_->send(client_id_, id, function(std::move(request)));
    }

    std::optional<AccountEvent> receive(std::chrono::milliseconds timeout) override {
        auto response = manager_->receive(static_cast<double>(timeout.count()) / 1000.0);
        if (!response.object) return {};
        AccountEvent event;
        event.request_id = response.request_id;
        const auto& object = *response.object;
        if (response.request_id == std::numeric_limits<std::uint64_t>::max()) {
            if (object.get_id() == api::error::ID) throw AccountError(AccountErrorCode::unavailable);
            event.request_id = 0;
        } else if (object.get_id() == api::updateAuthorizationState::ID) {
            const auto& state = static_cast<const api::updateAuthorizationState&>(object).authorization_state_;
            if (state) event.authorization = authorization(*state);
        } else if (object.get_id() == api::updateConnectionState::ID) {
            logger_.write(Event::connection_changed);
        } else if (response.request_id != 0) {
            const auto request = requests_.find(response.request_id);
            if (request == requests_.end()) return {};
            event.result = result(object, request->second);
            requests_.erase(request);
        }
        return event;
    }

private:
    api::object_ptr<api::Function> function(AccountRequest request) {
        switch (request.command) {
        case AccountCommand::parameters: return parameters();
        case AccountCommand::qr: return api::make_object<api::requestQrCodeAuthentication>(std::vector<std::int64_t>{});
        case AccountCommand::phone: return api::make_object<api::setAuthenticationPhoneNumber>(request.secret, nullptr);
        case AccountCommand::email: return api::make_object<api::setAuthenticationEmailAddress>(request.secret);
        case AccountCommand::email_code:
            return api::make_object<api::checkAuthenticationEmailCode>(api::make_object<api::emailAddressAuthenticationCode>(request.secret));
        case AccountCommand::code: return api::make_object<api::checkAuthenticationCode>(request.secret);
        case AccountCommand::password: return api::make_object<api::checkAuthenticationPassword>(request.secret);
        case AccountCommand::logout: return api::make_object<api::logOut>();
        case AccountCommand::close: return api::make_object<api::close>();
        case AccountCommand::load_chats: return api::make_object<api::loadChats>(chat_list(request.archive), request.limit);
        case AccountCommand::chat_ids: return api::make_object<api::getChats>(chat_list(request.archive), request.limit);
        case AccountCommand::chat: return api::make_object<api::getChat>(request.chat_id);
        case AccountCommand::search_messages:
        case AccountCommand::message:
        case AccountCommand::message_link: return make_tdlib_read_function(request);
        }
        throw AccountError(AccountErrorCode::invalid_input);
    }

    api::object_ptr<api::Function> parameters() {
        auto parameters = api::make_object<api::setTdlibParameters>();
        parameters->database_directory_ = (config_.state_directory / "database").string();
        parameters->files_directory_ = (config_.state_directory / "files").string();
        parameters->database_encryption_key_ = database_key_;
        parameters->use_chat_info_database_ = true;
        parameters->use_message_database_ = true;
        parameters->use_secret_chats_ = false;
        parameters->api_id_ = config_.api_id;
        parameters->api_hash_ = config_.api_hash;
        parameters->system_language_code_ = "en";
        parameters->device_model_ = "telegram-mcpp";
        parameters->application_version_ = "0.1.0";
        return parameters;
    }

    static api::object_ptr<api::ChatList> chat_list(bool archive) {
        if (archive) return api::make_object<api::chatListArchive>();
        return api::make_object<api::chatListMain>();
    }

    static AccountResult result(const api::Object& object, AccountCommand command) {
        if (object.get_id() != api::error::ID &&
            (command == AccountCommand::search_messages || command == AccountCommand::message ||
             command == AccountCommand::message_link)) return decode_tdlib_read_result(object, command);
        AccountResult result;
        switch (object.get_id()) {
        case api::ok::ID: break;
        case api::error::ID: {
            const auto& error = static_cast<const api::error&>(object);
            if (command == AccountCommand::load_chats && error.code_ == 404) result.exhausted = true;
            else result = decode_tdlib_error(error, command);
            break;
        }
        case api::chats::ID: result.chat_ids = static_cast<const api::chats&>(object).chat_ids_; break;
        case api::chat::ID: {
            result.chat = decode_tdlib_chat(static_cast<const api::chat&>(object));
            break;
        }
        default: return decode_tdlib_read_result(object, command);
        }
        return result;
    }

    Config config_;
    Logger& logger_;
    std::string database_key_;
    std::unique_ptr<td::ClientManager> manager_;
    td::ClientManager::ClientId client_id_;
    std::map<std::uint64_t, AccountCommand> requests_;
};

}

std::unique_ptr<AccountTransport> make_tdlib_transport(const Config& config, Logger& logger) {
    return std::make_unique<TdlibTransport>(config, logger);
}

}
