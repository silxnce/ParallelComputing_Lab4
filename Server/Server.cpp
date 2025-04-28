#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

using namespace std;
using namespace chrono;

// TLV header must stay 8 bytes with fixed 32-bit fields
struct TLVHeader {
    uint32_t type;   // Message type
    uint32_t length; // Payload length
};
static_assert(sizeof(TLVHeader) == 8, "TLVHeader must be 8 bytes");

// TLV message types
constexpr uint32_t TLV_CONFIG = 1;
constexpr uint32_t TLV_DATA = 2;
constexpr uint32_t TLV_COMPUTE = 3;
constexpr uint32_t TLV_STATUS = 4;
constexpr uint32_t TLV_RESULT = 5;
constexpr uint32_t TLV_ERROR = 255;

// Send exactly total_bytes from buffer over socket
bool send_all(SOCKET sock, const char* buffer, int total_bytes) {
    int sent = 0;
    while (sent < total_bytes) {
        int n = send(sock, buffer + sent, total_bytes - sent, 0);
        if (n == SOCKET_ERROR) return false;
        sent += n;
    }
    return true;
}

// Receive exactly total_bytes into buffer from socket
bool recv_all(SOCKET sock, char* buffer, int total_bytes) {
    int rec = 0;
    while (rec < total_bytes) {
        int n = recv(sock, buffer + rec, total_bytes - rec, 0);
        if (n <= 0) return false;
        rec += n;
    }
    return true;
}

// Build and send a TLV message
bool send_message(SOCKET sock, uint32_t type, const vector<char>& payload) {
    TLVHeader hdr = { htonl(type), htonl((uint32_t)payload.size()) };
    if (!send_all(sock, reinterpret_cast<char*>(&hdr), sizeof(hdr)))
        return false;
    if (!payload.empty() && !send_all(sock, payload.data(), (int)payload.size()))
        return false;
    return true;
}

// Read a TLV message into out_type and out_payload
bool recv_message(SOCKET sock, uint32_t& out_type, vector<char>& out_payload) {
    TLVHeader hdr;
    if (!recv_all(sock, reinterpret_cast<char*>(&hdr), sizeof(hdr)))
        return false;
    out_type = ntohl(hdr.type);
    uint32_t len = ntohl(hdr.length);
    out_payload.resize(len);
    return (len == 0) || recv_all(sock, out_payload.data(), len);
}

// Swap the two 32-bit halves of a 64-bit integer
inline uint64_t swap_64(uint64_t v) {
    uint32_t low = htonl(static_cast<uint32_t>(v & 0xFFFFFFFFULL));
    uint32_t high = htonl(static_cast<uint32_t>(v >> 32));
    return (static_cast<uint64_t>(low) << 32) | high;
}

void place_secondary_diagonal(vector<int>& mat, int N, int thread_count) {
    int workers = (thread_count > 0 && thread_count <= N) ? thread_count : 1;
    vector<thread> threads;
    auto worker = [&](int start_row, int end_row) {
        for (int i = start_row; i < end_row; ++i) {
            long long prod = 1;
            for (int j = 0; j < N; ++j) {
                prod *= mat[i * N + j];
            }
            mat[i * N + (N - 1 - i)] = static_cast<int>(prod);
        }
        };
    int rows_per_thread = N / workers;
    for (int t = 0; t < workers; ++t) {
        int start = t * rows_per_thread;
        int end = (t + 1 == workers ? N : start + rows_per_thread);
        threads.emplace_back(worker, start, end);
    }
    for (auto& th : threads) th.join();
}

/**
 * Handle one client:
 * 1) CONFIG  – receive N (int) and thread_count (int)
 * 2) DATA    – receive N×N matrix of int values
 * 3) COMPUTE – compute diagonal and measure time (double seconds)
 * 4) STATUS  – return status: 0=no data,1=working,2=done
 * 5) RESULT  – return [N(4B)][thread_count(4B)][time_sec(8B double)]
 */
void handle_client(SOCKET client_sock) {
    cout << "[+] Client connected: " << client_sock << "\n";

    int N = 0;
    int thread_count = 1;
    vector<int> matrix;
    bool has_data = false, computed = false;
    double time_sec = 0.0;

    uint32_t msg_type;
    vector<char> payload;

    while (recv_message(client_sock, msg_type, payload)) {
        switch (msg_type) {
        case TLV_CONFIG: {
            // payload = [4B N][4B thread_count]
            int32_t* p = reinterpret_cast<int32_t*>(payload.data());
            N = ntohl(static_cast<uint32_t>(p[0]));
            thread_count = ntohl(static_cast<uint32_t>(p[1]));
            cout << "    CONFIG: N=" << N
                << ", threads=" << thread_count << "\n";
            send_message(client_sock, TLV_CONFIG, {});  // ACK
            break;
        }
        case TLV_DATA: {
            // payload = N*N int32_t in network byte order
            matrix.resize(N * N);
            int32_t* p = reinterpret_cast<int32_t*>(payload.data());
            for (int i = 0; i < N * N; ++i) {
                matrix[i] = ntohl(static_cast<uint32_t>(p[i]));
            }
            has_data = true;
            cout << "    DATA received: matrix " << N << "x" << N << "\n";
            send_message(client_sock, TLV_DATA, {});  // ACK
            break;
        }
        case TLV_COMPUTE: {
            if (has_data) {
                auto t1 = high_resolution_clock::now();
                place_secondary_diagonal(matrix, N, thread_count);
                auto t2 = high_resolution_clock::now();
                time_sec = duration<double>(t2 - t1).count();
                computed = true;
                cout << "    COMPUTED in " << time_sec << " s\n";
            }
            send_message(client_sock, TLV_COMPUTE, {});  // ACK
            break;
        }
        case TLV_STATUS: {
            // 0=no data,1=ready/working,2=done
            char st = computed ? 2 : (has_data ? 1 : 0);
            send_message(client_sock, TLV_STATUS, vector<char>{st});
            break;
        }
        case TLV_RESULT: {
            if (computed) {
                // build payload [4B N][4B thread_count][8B time_sec]
                vector<char> out(16);
                int32_t net_n = htonl(N);
                int32_t net_th = htonl(thread_count);
                memcpy(out.data(), &net_n, 4);
                memcpy(out.data() + 4, &net_th, 4);
                static_assert(sizeof(double) == sizeof(uint64_t),
                    "double must be 8 bytes");
                uint64_t raw;
                memcpy(&raw, &time_sec, sizeof(raw));
                uint64_t net_raw = swap_64(raw);
                memcpy(out.data() + 8, &net_raw, 8);
                send_message(client_sock, TLV_RESULT, out);
                cout << "    RESULT sent\n";
            }
            else {
                send_message(client_sock, TLV_ERROR, {});
            }
            break;
        }
        default:
            send_message(client_sock, TLV_ERROR, {});
            break;
        }
    }

    cout << "[-] Client disconnected: " << client_sock << "\n";
    closesocket(client_sock);
}

int main() {
    // Initialize Winsock
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        cerr << "WSAStartup failed\n";
        return 1;
    }

    // Create listening socket
    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == INVALID_SOCKET) {
        cerr << "socket() error: " << WSAGetLastError() << "\n";
        WSACleanup();
        return 1;
    }

    // Bind to port 8888
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8888);
    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        cerr << "bind() error: " << WSAGetLastError() << "\n";
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    // Listen
    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        cerr << "listen() error: " << WSAGetLastError() << "\n";
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    cout << "Server running on port 8888\n";

    // Accept loop
    while (true) {
        SOCKET client_sock = accept(listen_sock, nullptr, nullptr);
        if (client_sock == INVALID_SOCKET) {
            cerr << "accept() error: " << WSAGetLastError() << "\n";
            continue;
        }
        thread(handle_client, client_sock).detach();
    }

    closesocket(listen_sock);
    WSACleanup();
    return 0;
}
