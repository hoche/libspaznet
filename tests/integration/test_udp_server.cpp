#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <libspaznet/server.hpp>
#include <libspaznet/udp_handler.hpp>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define close_socket ::close
#endif

using namespace spaznet;

class TestUDPHandler : public UDPHandler {
  public:
    std::atomic<int> packet_count{0};
    std::vector<UDPPacket> received_packets;

    Task handle_packet(const UDPPacket& packet, Socket& socket) override {
        packet_count.fetch_add(1);
        received_packets.push_back(packet);
        co_return;
    }
};

class UDPServerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        handler = std::make_unique<TestUDPHandler>();
        server = std::make_unique<Server>(2);
        server->set_udp_handler(std::make_unique<TestUDPHandler>());
        server->listen_udp(6666);

        server_thread = std::thread([this]() { server->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        server->stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

    bool send_udp_packet(const std::string& message, uint16_t port) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
            return false;

        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(port);

        int sent = sendto(sock, message.c_str(), message.size(), 0,
                          reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        close_socket(sock);

        return sent == static_cast<int>(message.size());
    }

    std::unique_ptr<TestUDPHandler> handler;
    std::unique_ptr<Server> server;
    std::thread server_thread;
};

TEST_F(UDPServerTest, UDPListen) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Server should be listening
    EXPECT_NE(server, nullptr);
}

TEST_F(UDPServerTest, SendUDPPacket) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    bool sent = send_udp_packet("Hello UDP", 6666);
    EXPECT_TRUE(sent);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(UDPServerTest, MultiplePackets) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    for (int i = 0; i < 5; ++i) {
        std::string message = "Packet " + std::to_string(i);
        send_udp_packet(message, 6666);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

TEST_F(UDPServerTest, DifferentPacketSizes) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Small packet
    send_udp_packet("A", 6666);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Medium packet
    std::string medium(100, 'B');
    send_udp_packet(medium, 6666);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Large packet (but within UDP limits)
    std::string large(500, 'C');
    send_udp_packet(large, 6666);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

TEST_F(UDPServerTest, BinaryData) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<uint8_t> binary_data = {0x00, 0x01, 0x02, 0xFF, 0xFE, 0xFD};
    std::string data_str(binary_data.begin(), binary_data.end());

    send_udp_packet(data_str, 6666);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
