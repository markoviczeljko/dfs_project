#include <gtest/gtest.h>

#include <optional>
#include <thread>

#include "network/connection.h"
#include "network/protocol.h"
#include "network/socket.h"

TEST(ProtocolHeader, RoundTripPreservesFields) {
    MessageHeader header{MAGIC, PROTOCOL_VERSION, MessageType::HEARTBEAT, 42, 7};
    auto bytes = Protocol::serialize_header(header);
    auto result =
        Protocol::deserialize_header(std::span<const std::byte, MESSAGE_HEADER_SIZE>(bytes));

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().magic, MAGIC);
    EXPECT_EQ(result.value().version, PROTOCOL_VERSION);
    EXPECT_EQ(result.value().message_type, MessageType::HEARTBEAT);
    EXPECT_EQ(result.value().request_id, 42u);
    EXPECT_EQ(result.value().payload_length, 7u);
}

TEST(ProtocolHeader, IsBigEndianOnWire) {
    MessageHeader header{MAGIC, PROTOCOL_VERSION, MessageType::ACK, 0x0102030405060708ULL,
                          0x11223344};
    auto bytes = Protocol::serialize_header(header);

    // magic 0x44534631 -> most significant byte first.
    EXPECT_EQ(static_cast<uint8_t>(bytes[0]), 0x44);
    EXPECT_EQ(static_cast<uint8_t>(bytes[1]), 0x53);
    EXPECT_EQ(static_cast<uint8_t>(bytes[2]), 0x46);
    EXPECT_EQ(static_cast<uint8_t>(bytes[3]), 0x31);

    // request_id 0x0102030405060708 -> most significant byte first.
    EXPECT_EQ(static_cast<uint8_t>(bytes[8]), 0x01);
    EXPECT_EQ(static_cast<uint8_t>(bytes[15]), 0x08);
}

TEST(ProtocolHeader, RejectsBadMagic) {
    MessageHeader header{0xDEADBEEF, PROTOCOL_VERSION, MessageType::ACK, 1, 0};
    auto bytes = Protocol::serialize_header(header);
    auto result =
        Protocol::deserialize_header(std::span<const std::byte, MESSAGE_HEADER_SIZE>(bytes));
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error(), ErrorCode::INVALID_MESSAGE);
}

TEST(ProtocolHeader, RejectsBadVersion) {
    MessageHeader header{MAGIC, 99, MessageType::ACK, 1, 0};
    auto bytes = Protocol::serialize_header(header);
    auto result =
        Protocol::deserialize_header(std::span<const std::byte, MESSAGE_HEADER_SIZE>(bytes));
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error(), ErrorCode::INVALID_MESSAGE);
}

TEST(TCPConnectionLoopback, RoundTripsMessage) {
    auto server_socket_result = Socket::create();
    ASSERT_TRUE(server_socket_result.is_ok());
    Socket server_socket = std::move(server_socket_result.value());

    constexpr uint16_t port = 34567;
    ASSERT_EQ(server_socket.bind(port), ErrorCode::OK);
    ASSERT_EQ(server_socket.listen(1), ErrorCode::OK);

    std::optional<Message> received;
    std::thread server_thread([&server_socket, &received]() {
        auto accepted = server_socket.accept();
        ASSERT_TRUE(accepted.is_ok());
        TCPConnection connection(std::move(accepted.value()));
        received = connection.receiveMessage();
    });

    auto client_socket_result = Socket::create();
    ASSERT_TRUE(client_socket_result.is_ok());
    Socket client_socket = std::move(client_socket_result.value());
    ASSERT_EQ(client_socket.connect("127.0.0.1", port), ErrorCode::OK);

    TCPConnection client_connection(std::move(client_socket));
    std::vector<std::byte> payload{std::byte{1}, std::byte{2}, std::byte{3}};
    Message message{MessageType::HEARTBEAT, 99, payload};
    ASSERT_TRUE(client_connection.sendMessage(message));

    server_thread.join();

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->type, MessageType::HEARTBEAT);
    EXPECT_EQ(received->request_id, 99u);
    EXPECT_EQ(received->payload, payload);
}

TEST(TCPConnectionLoopback, RejectsOversizedPayloadWithoutAllocating) {
    auto server_socket_result = Socket::create();
    ASSERT_TRUE(server_socket_result.is_ok());
    Socket server_socket = std::move(server_socket_result.value());

    constexpr uint16_t port = 34568;
    ASSERT_EQ(server_socket.bind(port), ErrorCode::OK);
    ASSERT_EQ(server_socket.listen(1), ErrorCode::OK);

    std::optional<Message> received;
    std::thread server_thread([&server_socket, &received]() {
        auto accepted = server_socket.accept();
        ASSERT_TRUE(accepted.is_ok());
        TCPConnection connection(std::move(accepted.value()));
        received = connection.receiveMessage();
    });

    auto client_socket_result = Socket::create();
    ASSERT_TRUE(client_socket_result.is_ok());
    Socket client_socket = std::move(client_socket_result.value());
    ASSERT_EQ(client_socket.connect("127.0.0.1", port), ErrorCode::OK);

    // Claim a payload_length far beyond MAX_CONTROL_MESSAGE_SIZE for a
    // control-plane message type, without ever sending that much data. If
    // receiveMessage() tried to recvAll() a buffer of that size, this test
    // would hang instead of completing.
    MessageHeader header{MAGIC, PROTOCOL_VERSION, MessageType::HEARTBEAT, 7,
                          static_cast<uint32_t>(MAX_CONTROL_MESSAGE_SIZE) + 1};
    auto header_bytes = Protocol::serialize_header(header);
    ssize_t sent = client_socket.send(header_bytes.data(), header_bytes.size());
    ASSERT_EQ(sent, static_cast<ssize_t>(header_bytes.size()));

    server_thread.join();

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->type, MessageType::ERROR);
    EXPECT_EQ(received->request_id, 7u);
}

// Faza 18: ABANDON_BLOCKS carries an unbounded block-id list, same shape as
// NODE_INVENTORY — it must get the data-message size limit, or a failed
// write that allocated thousands of blocks would exceed the 64KB control
// limit and be rejected mid-stream.
TEST(ProtocolLimits, AbandonBlocksUsesDataMessageLimit) {
    EXPECT_TRUE(is_data_message(MessageType::ABANDON_BLOCKS));
    EXPECT_TRUE(is_data_message(MessageType::NODE_INVENTORY));
}
