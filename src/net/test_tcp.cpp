// =============================================================================
// TCP + FileTransfer Test
// =============================================================================
#include "core/tcp_socket.h"
#include "core/file_transfer.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <cstring>

using namespace net;

void test_tcp_basic() {
    std::cout << "=== TCP Socket Basic Test ===\n";

    // Create test file
    const char* test_file = "test_data.bin";
    {
        std::ofstream f(test_file, std::ios::binary);
        for (int i = 0; i < 10000; i++) {
            f.put((char)(i % 256));
        }
    }

    // Server thread
    std::thread server_thread([]() {
        TCPSocket server;
        if (!server.create()) {
            std::cout << "Server create failed\n";
            return;
        }
        if (!server.bind_port(27016)) {
            std::cout << "Server bind failed\n";
            return;
        }
        if (!server.listen()) {
            std::cout << "Server listen failed\n";
            return;
        }
        std::cout << "Server listening on port 27016...\n";

        TCPSocket* client = server.accept();
        if (!client) {
            std::cout << "Server accept failed\n";
            return;
        }
        std::cout << "Server accepted connection\n";

        // Send file
        bool ok = FileTransfer::sendFile(*client, "test_data.bin", 
            [](size_t sent, size_t total) {
                std::cout << "Server: sent " << sent << "/" << total << " bytes\r" << std::flush;
            });
        std::cout << "\nServer: send " << (ok ? "OK" : "FAIL") << "\n";

        delete client;
    });

    // Client thread
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // wait for server
    std::thread client_thread([]() {
        TCPSocket client;
        if (!client.create()) {
            std::cout << "Client create failed\n";
            return;
        }
        if (!client.connect("127.0.0.1", 27016)) {
            std::cout << "Client connect failed\n";
            return;
        }
        std::cout << "Client connected\n";

        // Receive file
        bool ok = FileTransfer::receiveFile(client, "test_data_received.bin",
            [](size_t received, size_t total) {
                std::cout << "Client: received " << received << "/" << total << " bytes\r" << std::flush;
            });
        std::cout << "\nClient: receive " << (ok ? "OK" : "FAIL") << "\n";
    });

    server_thread.join();
    client_thread.join();

    // Verify
    std::ifstream f1("test_data.bin", std::ios::binary | std::ios::ate);
    std::ifstream f2("test_data_received.bin", std::ios::binary | std::ios::ate);
    if (f1.tellg() == f2.tellg()) {
        f1.seekg(0); f2.seekg(0);
        std::vector<uint8_t> data1((size_t)f1.tellg());
        std::vector<uint8_t> data2((size_t)f2.tellg());
        f1.seekg(0); f2.seekg(0);
        f1.read((char*)data1.data(), data1.size());
        f2.read((char*)data2.data(), data2.size());
        if (std::memcmp(data1.data(), data2.data(), data1.size()) == 0) {
            std::cout << "File transfer verification: OK ✅\n";
        } else {
            std::cout << "File transfer verification: FAIL (content mismatch) ❌\n";
        }
    } else {
        std::cout << "File transfer verification: FAIL (size mismatch) ❌\n";
    }

    std::cout << "\n";
}

void test_tcp_large_file() {
    std::cout << "=== TCP Large File Test ===\n";

    // Create large test file (10 MB)
    const char* test_file = "test_large.bin";
    {
        std::ofstream f(test_file, std::ios::binary);
        for (int i = 0; i < 10 * 1024 * 1024; i++) {
            f.put((char)(i % 256));
        }
    }
    std::cout << "Created 10 MB test file\n";

    // Server thread
    std::thread server_thread([]() {
        TCPSocket server;
        server.create();
        server.bind_port(27017);
        server.listen();

        TCPSocket* client = server.accept();
        if (client) {
            auto start = std::chrono::high_resolution_clock::now();
            FileTransfer::sendFile(*client, "test_large.bin");
            auto end = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            std::cout << "Server: sent 10 MB in " << ms << " ms\n";
            delete client;
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::thread client_thread([]() {
        TCPSocket client;
        client.create();
        client.connect("127.0.0.1", 27017);

        auto start = std::chrono::high_resolution_clock::now();
        FileTransfer::receiveFile(client, "test_large_received.bin");
        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "Client: received 10 MB in " << ms << " ms\n";
    });

    server_thread.join();
    client_thread.join();

    std::ifstream f1("test_large.bin", std::ios::binary | std::ios::ate);
    std::ifstream f2("test_large_received.bin", std::ios::binary | std::ios::ate);
    if (f1.tellg() == f2.tellg() && f1.tellg() == 10 * 1024 * 1024) {
        std::cout << "Large file transfer: OK ✅\n";
    } else {
        std::cout << "Large file transfer: FAIL ❌\n";
    }

    std::cout << "\n";
}

int main() {
    std::cout << "=== TCP + FileTransfer Test ===\n\n";
    
    // Initialize network
    #ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    #endif

    test_tcp_basic();
    test_tcp_large_file();

    #ifdef _WIN32
    WSACleanup();
    #endif

    std::cout << "=== All TCP tests complete ===\n";
    return 0;
}
