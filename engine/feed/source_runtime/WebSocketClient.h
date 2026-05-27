#pragma once

#include <string>

namespace trading_engine::feed {

class WebSocketClient {
public:
    explicit WebSocketClient(std::string endpoint = {});

    void set_endpoint(std::string endpoint);
    void connect();
    void disconnect() noexcept;

    [[nodiscard]] const std::string& endpoint() const noexcept;
    [[nodiscard]] bool connected() const noexcept;

private:
    std::string endpoint_;
    bool connected_{false};
};

}  // namespace trading_engine::feed
