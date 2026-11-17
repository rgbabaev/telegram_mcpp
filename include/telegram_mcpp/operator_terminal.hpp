#pragma once

#include "telegram_mcpp/account.hpp"

namespace telegram_mcpp {

class OperatorTerminal final : public AccountPrompt {
public:
    explicit OperatorTerminal(Cancelled cancelled);
    ~OperatorTerminal();
    std::string secret(const std::string& prompt) override;
    void qr(const std::string& link) override;
    void rejected() override;
private:
    void write(const std::string& value);
    int terminal_;
    Cancelled cancelled_;
};

}
