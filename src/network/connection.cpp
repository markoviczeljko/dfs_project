#include "network/connection.h"

#include <array>
#include <cstring>

bool TCPConnection::sendAll(const std::byte* data, size_t size) {
    size_t remaining = size;
    const std::byte* cursor = data;
    while (remaining > 0) {
        ssize_t n = socket_.send(cursor, remaining);
        if (n <= 0) {
            return false;
        }
        remaining -= static_cast<size_t>(n);
        cursor += n;
    }
    return true;
}

bool TCPConnection::recvAll(std::byte* buffer, size_t size) {
    size_t remaining = size;
    std::byte* cursor = buffer;
    while (remaining > 0) {
        ssize_t n = socket_.recv(cursor, remaining);
        if (n <= 0) {
            return false;
        }
        remaining -= static_cast<size_t>(n);
        cursor += n;
    }
    return true;
}

bool TCPConnection::sendMessage(const Message& message) {
    std::vector<std::byte> bytes = Protocol::serialize(message);
    return sendAll(bytes.data(), bytes.size());
}

std::optional<Message> TCPConnection::receiveMessage() {
    std::array<std::byte, MESSAGE_HEADER_SIZE> header_bytes{};
    if (!recvAll(header_bytes.data(), header_bytes.size())) {
        return std::nullopt;
    }

    auto header_result =
        Protocol::deserialize_header(std::span<const std::byte, MESSAGE_HEADER_SIZE>(header_bytes));
    if (header_result.is_error()) {
        return std::nullopt;
    }

    const MessageHeader& header = header_result.value();
    size_t limit =
        is_data_message(header.message_type) ? MAX_DATA_MESSAGE_SIZE : MAX_CONTROL_MESSAGE_SIZE;

    if (header.payload_length > limit) {
        // Reject based on the claimed length alone — never allocate a buffer
        // sized by an untrusted, oversized payload_length.
        std::vector<std::byte> error_payload(sizeof(uint16_t));
        uint16_t code = static_cast<uint16_t>(ErrorCode::INVALID_MESSAGE);
        std::memcpy(error_payload.data(), &code, sizeof(code));
        return Message{MessageType::ERROR, header.request_id, std::move(error_payload)};
    }

    std::vector<std::byte> payload(header.payload_length);
    if (header.payload_length > 0 && !recvAll(payload.data(), payload.size())) {
        return std::nullopt;
    }

    return Message{header.message_type, header.request_id, std::move(payload)};
}
