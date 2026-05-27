#include "feed/source_runtime/WebSocketClient.h"

#include <utility>

namespace trading_engine::feed {

WebSocketClient::WebSocketClient(std::string endpoint)
    : endpoint_(std::move(endpoint)) {}

void WebSocketClient::set_endpoint(std::string endpoint) {
    endpoint_ = std::move(endpoint);
}

void WebSocketClient::connect() {
    connected_ = !endpoint_.empty();
}

void WebSocketClient::disconnect() noexcept {
    connected_ = false;
}

const std::string& WebSocketClient::endpoint() const noexcept {
    return endpoint_;
}

bool WebSocketClient::connected() const noexcept {
    return connected_;
}

}  // namespace trading_engine::feed
