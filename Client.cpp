#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <vector>
#include <random>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

using namespace std;
using namespace chrono;

// TLV message types
constexpr uint32_t TLV_CONFIG = 1;
constexpr uint32_t TLV_DATA = 2;
constexpr uint32_t TLV_COMPUTE = 3;
constexpr uint32_t TLV_STATUS = 4;
constexpr uint32_t TLV_RESULT = 5;
constexpr uint32_t TLV_ERROR = 255;

// Swap the two 32-bit halves of a 64-bit integer
inline uint64_t swap_64(uint64_t v) {
    uint32_t low = ntohl(static_cast<uint32_t>(v & 0xFFFFFFFFULL));
    uint32_t high = ntohl(static_cast<uint32_t>(v >> 32));
    return (static_cast<uint64_t>(low) << 32) | high;
}

// Send exactly total_bytes from buffer over socket
int send_all(SOCKET sock, const char* buffer, int total_bytes) {
    int sent = 0;
    while (sent < total_bytes) {
        int n = send(sock, buffer + sent, total_bytes - sent, 0);
        if (n == SOCKET_ERROR) return SOCKET_ERROR;
        sent += n;
    }
    return sent;
}

// Receive exactly total_bytes into buffer from socket
int recv_all(SOCKET sock, char* buffer, int total_bytes) {
    int rec = 0;
    while (rec < total_bytes) {
        int n = recv(sock, buffer + rec, total_bytes - rec, 0);
        if (n <= 0) return n;
        rec += n;
    }
    return rec;
}

// Send a TLV packet
bool send_tlv(SOCKET sock, uint32_t type, const vector<char>& payload) {
    uint32_t hdr[2] = { htonl(type), htonl(static_cast<uint32_t>(payload.size())) };
    if (send_all(sock, reinterpret_cast<char*>(hdr), sizeof(hdr)) != sizeof(hdr))
        return false;
    if (!payload.empty()) {
        if (send_all(sock, payload.data(), static_cast<int>(payload.size())) != static_cast<int>(payload.size()))
            return false;
    }
    return true;
}

// Receive a TLV packet into out_type and out_payload.
bool recv_tlv(SOCKET sock, uint32_t& out_type, vector<char>& out_payload) {
    uint32_t hdr[2];
    if (recv_all(sock, reinterpret_cast<char*>(hdr), sizeof(hdr)) != sizeof(hdr))
        return false;
    out_type = ntohl(hdr[0]);
    uint32_t len = ntohl(hdr[1]);
    out_payload.resize(len);
    if (len > 0) {
        if (recv_all(sock, out_payload.data(), static_cast<int>(len)) != static_cast<int>(len))
            return false;
    }
    return true;
}

int main() {
    // Initialize Winsock
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        cerr << "WSAStartup failed\n";
        return 1;
    }

    // Create TCP socket
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        cerr << "socket() error: " << WSAGetLastError() << "\n";
        WSACleanup();
        return 1;
    }

    // Connect to server at localhost:8888
    sockaddr_in srv_addr{};
    srv_addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &srv_addr.sin_addr);
    srv_addr.sin_port = htons(8888);
    if (connect(sock, reinterpret_cast<sockaddr*>(&srv_addr), sizeof(srv_addr)) == SOCKET_ERROR) {
        cerr << "connect() error: " << WSAGetLastError() << "\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // Send a command and wait for its ACK
    auto do_ack = [&](uint32_t cmd, const vector<char>& payload) {
        send_tlv(sock, cmd, payload);
        uint32_t resp;
        vector<char> dummy;
        recv_tlv(sock, resp, dummy);  // ignore ACK contents
        };

    // Step 1: CONFIG
    int matrix_size = 10000;
    int thread_count = 128;
    // Pack into 8-byte payload
    vector<char> cfg(8);
    int32_t* pcfg = reinterpret_cast<int32_t*>(cfg.data());
    pcfg[0] = htonl(matrix_size);
    pcfg[1] = htonl(thread_count);
    do_ack(TLV_CONFIG, cfg);

    // Step 2: DATA
    // Generate random matrix of int
    vector<int> matrix(matrix_size * matrix_size);
    mt19937 rng(random_device{}());
    uniform_int_distribution<int> dist(1, 10);
    for (auto& v : matrix) v = dist(rng);

    // Serialize into network-order int32
    vector<char> data(matrix.size() * 4);
    int32_t* pdata = reinterpret_cast<int32_t*>(data.data());
    for (size_t i = 0; i < matrix.size(); ++i) {
        pdata[i] = htonl(matrix[i]);
    }
    do_ack(TLV_DATA, data);

    // Step 3: COMPUTE
    do_ack(TLV_COMPUTE, {});

    // Step 4: STATUS
    // Poll until server returns status=2
    while (true) {
        send_tlv(sock, TLV_STATUS, {});
        uint32_t st_type;
        vector<char> st_payload;
        recv_tlv(sock, st_type, st_payload);
        if (st_type == TLV_STATUS && !st_payload.empty() && st_payload[0] == 2)
            break;
        this_thread::sleep_for(chrono::milliseconds(100));
    }

    // Step 5: RESULT
    send_tlv(sock, TLV_RESULT, {});
    uint32_t res_type;
    vector<char> res_payload;
    if (recv_tlv(sock, res_type, res_payload)
        && res_type == TLV_RESULT
        && res_payload.size() >= 16)
    {
        // Unpack [4B N][4B threads][8B time_sec]
        uint32_t net_n = *reinterpret_cast<uint32_t*>(res_payload.data() + 0);
        uint32_t net_th = *reinterpret_cast<uint32_t*>(res_payload.data() + 4);
        uint64_t net_raw = *reinterpret_cast<uint64_t*>(res_payload.data() + 8);

        int server_n = ntohl(net_n);
        int server_threads = ntohl(net_th);

        // Convert raw bits to double
        uint64_t raw;
        raw = swap_64(net_raw);
        double server_time;
        memcpy(&server_time, &raw, sizeof(raw));

        cout << "Server processed:\n";
        cout << "  matrix size  = " << server_n << "\n";
        cout << "  threads used = " << server_threads << "\n";
        cout << "  time elapsed = " << server_time << " s\n";
    }
    else {
        cerr << "Failed to receive RESULT\n";
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
