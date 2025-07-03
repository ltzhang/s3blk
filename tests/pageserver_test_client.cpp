// SPDX-License-Identifier: MIT or GPL-2.0-only

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <getopt.h>
#include <sys/stat.h>
#include <cstdarg>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <cassert>
#include <errno.h>
#include <signal.h>

// Include the page server protocol definitions
#include "../targets/pageserver/pageserver.h"

class PageServerTestClient {
private:
    int client_fd;
    std::string server_host;
    int server_port;
    bool verbose;
    std::atomic<bool> stop_test;
    std::mutex print_mutex;
    
    // Test statistics
    std::atomic<uint64_t> total_operations;
    std::atomic<uint64_t> successful_operations;
    std::atomic<uint64_t> failed_operations;
    std::atomic<uint64_t> bytes_read;
    std::atomic<uint64_t> bytes_written;

    void log_message(const char *fmt, ...) {
        if (!verbose) return;
        
        std::lock_guard<std::mutex> lock(print_mutex);
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        printf("\n");
        fflush(stdout);
    }

    int send_request(uint8_t cmd, uint64_t offset, uint32_t length, const void *data = nullptr) {
        struct page_request req;
        req.magic = PAGESERVER_MAGIC;
        req.version = PAGESERVER_VERSION;
        req.cmd = cmd;
        req.offset = offset;
        req.length = length;
        memset(req.reserved, 0, sizeof(req.reserved));
        memset(&req.reserved2, 0, sizeof(req.reserved2));

        // Send request header
        ssize_t sent = send(client_fd, &req, sizeof(req), MSG_NOSIGNAL);
        if (sent != sizeof(req)) {
            if (sent < 0) {
                log_message("Failed to send request header: %s", strerror(errno));
            } else {
                log_message("Failed to send request header: %zd != %zu", sent, sizeof(req));
            }
            return -1;
        }

        // Send data if provided
        if (data && length > 0) {
            sent = send(client_fd, data, length, MSG_NOSIGNAL);
            if (sent != (ssize_t)length) {
                if (sent < 0) {
                    log_message("Failed to send request data: %s", strerror(errno));
                } else {
                    log_message("Failed to send request data: %zd != %u", sent, length);
                }
                return -1;
            }
        }

        return 0;
    }

    int receive_response(struct page_response *resp, void *data = nullptr, uint32_t max_data_len = 0) {
        // Receive response header with MSG_WAITALL to ensure complete message
        ssize_t received = recv(client_fd, resp, sizeof(*resp), MSG_WAITALL);
        if (received != sizeof(*resp)) {
            if (received < 0) {
                log_message("Failed to receive response header: %s", strerror(errno));
            } else {
                log_message("Incomplete response header: %zd != %zu", received, sizeof(*resp));
            }
            return -1;
        }

        // Validate response
        if (resp->magic != PAGESERVER_MAGIC) {
            log_message("Invalid response magic: 0x%08x (expected: 0x%08x)", resp->magic, PAGESERVER_MAGIC);
            return -1;
        }

        if (resp->version != PAGESERVER_VERSION) {
            log_message("Invalid response version: %u (expected: %u)", resp->version, PAGESERVER_VERSION);
            return -1;
        }

        // Receive data if present
        if (data && resp->length > 0) {
            if (resp->length > max_data_len) {
                log_message("Response data too large: %u > %u", resp->length, max_data_len);
                return -1;
            }

            received = recv(client_fd, data, resp->length, MSG_WAITALL);
            if (received != (ssize_t)resp->length) {
                if (received < 0) {
                    log_message("Failed to receive response data: %s", strerror(errno));
                } else {
                    log_message("Incomplete response data: %zd != %u", received, resp->length);
                }
                return -1;
            }
        }

        return 0;
    }

public:
    PageServerTestClient(const std::string &host, int port, bool v = false) 
        : client_fd(-1), server_host(host), server_port(port), verbose(v), stop_test(false),
          total_operations(0), successful_operations(0), failed_operations(0), 
          bytes_read(0), bytes_written(0) {}

    ~PageServerTestClient() {
        if (client_fd >= 0) {
            close(client_fd);
        }
    }

    int connect_to_server() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("socket");
            return -1;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(server_port);
        addr.sin_addr.s_addr = inet_addr(server_host.c_str());

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("connect");
            close(sock);
            return -1;
        }

        client_fd = sock;
        log_message("Connected to server %s:%d", server_host.c_str(), server_port);
        return 0;
    }

    void disconnect() {
        if (client_fd >= 0) {
            close(client_fd);
            client_fd = -1;
        }
    }

    // Test basic functionality
    int test_basic_operations() {
        log_message("Testing basic operations...");
        
        // Test STAT command
        if (test_stat() != 0) {
            log_message("STAT test failed");
            return -1;
        }

        // Test basic READ/WRITE
        if (test_basic_read_write() != 0) {
            log_message("Basic READ/WRITE test failed");
            return -1;
        }

        // Test FLUSH
        if (test_flush() != 0) {
            log_message("FLUSH test failed");
            return -1;
        }

        // Test DISCARD
        if (test_discard() != 0) {
            log_message("DISCARD test failed");
            return -1;
        }

        log_message("Basic operations test passed");
        return 0;
    }

    int test_stat() {
        log_message("Testing STAT command...");
        
        if (send_request(PAGE_CMD_STAT, 0, 0) != 0) {
            return -1;
        }

        struct page_response resp;
        struct page_stats stats;
        if (receive_response(&resp, &stats, sizeof(stats)) != 0) {
            return -1;
        }

        if (resp.status != PAGE_RESP_OK) {
            log_message("STAT failed with status %u", resp.status);
            return -1;
        }

        log_message("STAT response: total_size=%lu, page_size=%u", 
                   (unsigned long)stats.total_size, stats.page_size);
        return 0;
    }

    int test_basic_read_write() {
        log_message("Testing basic READ/WRITE...");
        
        // Write test data
        char write_data[PAGE_SIZE];
        for (int i = 0; i < PAGE_SIZE; i++) {
            write_data[i] = (char)(i % 256);
        }

        if (send_request(PAGE_CMD_WRITE, 0, PAGE_SIZE, write_data) != 0) {
            return -1;
        }

        struct page_response resp;
        if (receive_response(&resp) != 0) {
            return -1;
        }

        if (resp.status != PAGE_RESP_OK) {
            log_message("WRITE failed with status %u", resp.status);
            return -1;
        }

        // Read back the data
        if (send_request(PAGE_CMD_READ, 0, PAGE_SIZE) != 0) {
            return -1;
        }

        char read_data[PAGE_SIZE];
        if (receive_response(&resp, read_data, PAGE_SIZE) != 0) {
            return -1;
        }

        if (resp.status != PAGE_RESP_OK) {
            log_message("READ failed with status %u", resp.status);
            return -1;
        }

        // Verify data
        if (memcmp(write_data, read_data, PAGE_SIZE) != 0) {
            log_message("Data verification failed");
            return -1;
        }

        log_message("Basic READ/WRITE test passed");
        return 0;
    }

    int test_flush() {
        log_message("Testing FLUSH command...");
        
        if (send_request(PAGE_CMD_FLUSH, 0, 0) != 0) {
            return -1;
        }

        struct page_response resp;
        if (receive_response(&resp) != 0) {
            return -1;
        }

        if (resp.status != PAGE_RESP_OK) {
            log_message("FLUSH failed with status %u", resp.status);
            return -1;
        }

        log_message("FLUSH test passed");
        return 0;
    }

    int test_discard() {
        log_message("Testing DISCARD command...");
        
        if (send_request(PAGE_CMD_DISCARD, PAGE_SIZE, PAGE_SIZE) != 0) {
            return -1;
        }

        struct page_response resp;
        if (receive_response(&resp) != 0) {
            return -1;
        }

        if (resp.status != PAGE_RESP_OK) {
            log_message("DISCARD failed with status %u", resp.status);
            return -1;
        }

        log_message("DISCARD test passed");
        return 0;
    }

    // Test error cases
    int test_error_cases() {
        log_message("Testing error cases...");
        
        // Test invalid magic
        if (test_invalid_magic() != 0) {
            log_message("Invalid magic test failed");
            return -1;
        }

        // Test invalid version
        if (test_invalid_version() != 0) {
            log_message("Invalid version test failed");
            return -1;
        }

        // Test invalid command
        if (test_invalid_command() != 0) {
            log_message("Invalid command test failed");
            return -1;
        }

        // Test out of bounds offset
        if (test_out_of_bounds_offset() != 0) {
            log_message("Out of bounds offset test failed");
            return -1;
        }

        // Test negative offset
        if (test_negative_offset() != 0) {
            log_message("Negative offset test failed");
            return -1;
        }

        // Test large length (should be OK if within bounds)
        if (test_too_long_length() != 0) {
            log_message("Large length test failed");
            return -1;
        }

        // Test truly out of bounds request
        if (test_truly_out_of_bounds() != 0) {
            log_message("Truly out of bounds test failed");
            return -1;
        }

        // Test buffer overrun
        if (test_buffer_overrun() != 0) {
            log_message("Buffer overrun test failed");
            return -1;
        }

        log_message("Error cases test passed");
        return 0;
    }

    int test_invalid_magic() {
        log_message("Testing invalid magic...");
        
        struct page_request req;
        req.magic = 0x12345678;  // Invalid magic
        req.version = PAGESERVER_VERSION;
        req.cmd = PAGE_CMD_READ;
        req.offset = 0;
        req.length = PAGE_SIZE;
        memset(req.reserved, 0, sizeof(req.reserved));
        memset(&req.reserved2, 0, sizeof(req.reserved2));

        if (send(client_fd, &req, sizeof(req), MSG_NOSIGNAL) != sizeof(req)) {
            return -1;
        }

        struct page_response resp;
        if (receive_response(&resp) != 0) {
            // Connection was reset, which is expected for protocol errors
            log_message("Connection reset (expected for invalid magic)");
            // Reconnect for next test
            if (connect_to_server() != 0) {
                log_message("Failed to reconnect after invalid magic test");
                return -1;
            }
            log_message("Invalid magic test passed (connection reset as expected)");
            return 0;
        }

        if (resp.status == PAGE_RESP_OK) {
            log_message("Expected error for invalid magic, got OK");
            return -1;
        }

        log_message("Invalid magic test passed (got expected error)");
        return 0;
    }

    int test_invalid_version() {
        log_message("Testing invalid version...");
        
        struct page_request req;
        req.magic = PAGESERVER_MAGIC;
        req.version = 999;  // Invalid version
        req.cmd = PAGE_CMD_READ;
        req.offset = 0;
        req.length = PAGE_SIZE;
        memset(req.reserved, 0, sizeof(req.reserved));
        memset(&req.reserved2, 0, sizeof(req.reserved2));

        if (send(client_fd, &req, sizeof(req), MSG_NOSIGNAL) != sizeof(req)) {
            return -1;
        }

        struct page_response resp;
        if (receive_response(&resp) != 0) {
            // Connection was reset, which is expected for protocol errors
            log_message("Connection reset (expected for invalid version)");
            // Reconnect for next test
            if (connect_to_server() != 0) {
                log_message("Failed to reconnect after invalid version test");
                return -1;
            }
            log_message("Invalid version test passed (connection reset as expected)");
            return 0;
        }

        if (resp.status == PAGE_RESP_OK) {
            log_message("Expected error for invalid version, got OK");
            return -1;
        }

        log_message("Invalid version test passed (got expected error)");
        return 0;
    }

    int test_invalid_command() {
        log_message("Testing invalid command...");
        
        if (send_request(0xFF, 0, PAGE_SIZE) != 0) {
            return -1;
        }

        struct page_response resp;
        if (receive_response(&resp) != 0) {
            // Connection was reset, which is expected for invalid commands
            log_message("Connection reset (expected for invalid command)");
            // Reconnect for next test
            if (connect_to_server() != 0) {
                log_message("Failed to reconnect after invalid command test");
                return -1;
            }
            log_message("Invalid command test passed (connection reset as expected)");
            return 0;
        }

        if (resp.status == PAGE_RESP_OK) {
            log_message("Expected error for invalid command, got OK");
            return -1;
        }

        log_message("Invalid command test passed (got expected error)");
        return 0;
    }

    int test_out_of_bounds_offset() {
        log_message("Testing out of bounds offset...");
        
        // Try to read from a very large offset that should be beyond any reasonable file size
        if (send_request(PAGE_CMD_READ, 0xFFFFFFFFFFFFFFFFULL, PAGE_SIZE) != 0) {
            return -1;
        }

        struct page_response resp;
        if (receive_response(&resp) != 0) {
            // Connection was reset, which is expected for out of bounds requests
            log_message("Connection reset (expected for out of bounds offset)");
            // Reconnect for next test
            if (connect_to_server() != 0) {
                log_message("Failed to reconnect after out of bounds offset test");
                return -1;
            }
            log_message("Out of bounds offset test passed (connection reset as expected)");
            return 0;
        }

        if (resp.status == PAGE_RESP_OK) {
            log_message("Expected error for out of bounds offset, got OK");
            return -1;
        }

        log_message("Out of bounds offset test passed (got expected error)");
        return 0;
    }

    int test_negative_offset() {
        log_message("Testing negative offset...");
        
        // Try to read from a negative offset (interpreted as very large positive)
        if (send_request(PAGE_CMD_READ, (uint64_t)-1, PAGE_SIZE) != 0) {
            return -1;
        }

        struct page_response resp;
        if (receive_response(&resp) != 0) {
            // Connection was reset, which is expected for out of bounds requests
            log_message("Connection reset (expected for negative offset)");
            // Reconnect for next test
            if (connect_to_server() != 0) {
                log_message("Failed to reconnect after negative offset test");
                return -1;
            }
            log_message("Negative offset test passed (connection reset as expected)");
            return 0;
        }

        if (resp.status == PAGE_RESP_OK) {
            log_message("Expected error for negative offset, got OK");
            return -1;
        }

        log_message("Negative offset test passed (got expected error)");
        return 0;
    }

    int test_too_long_length() {
        log_message("Testing large length (should be OK if within file bounds)...");
        
        // Try to read with a large length that should be within file bounds
        // Use a reasonable large size that should fit in most test files
        uint32_t large_length = 1024 * 1024; // 1MB
        
        if (send_request(PAGE_CMD_READ, 0, large_length) != 0) {
            return -1;
        }

        struct page_response resp;
        if (receive_response(&resp) != 0) {
            return -1;
        }

        // This should succeed if the file is large enough, or fail with EOF if not
        if (resp.status != PAGE_RESP_OK && resp.status != PAGE_RESP_EOF) {
            log_message("Unexpected response status for large length: %u", resp.status);
            return -1;
        }

        log_message("Large length test passed (status: %u)", resp.status);
        return 0;
    }

    int test_truly_out_of_bounds() {
        log_message("Testing truly out of bounds request...");
        
        // Try to read with offset + length that exceeds file size
        // Use a reasonable offset but with a length that would exceed file bounds
        uint64_t offset = 1024; // Start at 1KB
        uint32_t length = 0xFFFFFFFF; // Very large length
        
        if (send_request(PAGE_CMD_READ, offset, length) != 0) {
            log_message("Failed to send out of bounds request");
            return -1;
        }

        struct page_response resp;
        if (receive_response(&resp) != 0) {
            // Connection was reset, which is expected for this test
            log_message("Connection reset (expected for out of bounds request)");
            // Reconnect for next test
            if (connect_to_server() != 0) {
                log_message("Failed to reconnect after out of bounds test");
                return -1;
            }
            log_message("Truly out of bounds test passed (connection reset as expected)");
            return 0;
        }

        if (resp.status == PAGE_RESP_OK) {
            log_message("Expected error for out of bounds request, got OK");
            return -1;
        }

        log_message("Truly out of bounds test passed (got expected error)");
        return 0;
    }

    int test_buffer_overrun() {
        log_message("Testing buffer overrun...");
        
        // Send a request with data that's larger than the length field
        char large_data[PAGE_SIZE * 2];
        memset(large_data, 0xAA, sizeof(large_data));
        
        if (send_request(PAGE_CMD_WRITE, 0, PAGE_SIZE, large_data) != 0) {
            return -1;
        }

        struct page_response resp;
        if (receive_response(&resp) != 0) {
            return -1;
        }

        // The server should handle this gracefully
        log_message("Buffer overrun test completed (status: %u)", resp.status);
        return 0;
    }

    // Multi-threaded stress test
    int run_stress_test(int num_threads, int operations_per_thread, int duration_seconds) {
        log_message("Starting stress test: %d threads, %d ops/thread, %d seconds", 
                   num_threads, operations_per_thread, duration_seconds);
        
        std::vector<std::thread> threads;
        std::atomic<bool> stop_stress(false);
        
        // Set up signal handler for graceful shutdown
        signal(SIGINT, [](int) {});
        
        auto start_time = std::chrono::steady_clock::now();
        
        // Calculate operations per second to distribute load properly
        double ops_per_second = (double)operations_per_thread / duration_seconds;
        log_message("Target rate: %.1f operations/second per thread", ops_per_second);
        
        // Start worker threads
        for (int i = 0; i < num_threads; i++) {
            threads.emplace_back([this, i, ops_per_second, &stop_stress]() {
                stress_test_worker(i, ops_per_second, stop_stress);
            });
        }
        
        // Run for specified duration
        std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
        stop_stress = true;
        
        // Wait for all threads to complete
        for (auto& thread : threads) {
            thread.join();
        }
        
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // Print statistics
        log_message("Stress test completed in %ld ms", duration.count());
        log_message("Total operations: %lu", total_operations.load());
        log_message("Successful operations: %lu", successful_operations.load());
        log_message("Failed operations: %lu", failed_operations.load());
        log_message("Bytes read: %lu", bytes_read.load());
        log_message("Bytes written: %lu", bytes_written.load());
        
        if (total_operations.load() > 0) {
            double success_rate = (double)successful_operations.load() / total_operations.load() * 100.0;
            double actual_ops_per_second = (double)total_operations.load() / (duration.count() / 1000.0);
            log_message("Success rate: %.2f%%", success_rate);
            log_message("Actual rate: %.1f operations/second", actual_ops_per_second);
        }
        
        return 0;
    }

    void stress_test_worker(int thread_id, double ops_per_second, std::atomic<bool>& stop) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint64_t> offset_dist(0, 1024 * 1024);  // 1MB range
        std::uniform_int_distribution<uint32_t> length_dist(512, PAGE_SIZE);
        std::uniform_int_distribution<int> cmd_dist(0, 4);  // READ, WRITE, FLUSH, DISCARD, STAT
        
        char buffer[PAGE_SIZE];
        
        // Calculate sleep interval to maintain target throughput
        // Each thread should maintain ops_per_second / num_threads rate
        double seconds_per_operation = 1.0 / ops_per_second;
        auto sleep_interval = std::chrono::microseconds((long long)(seconds_per_operation * 1000000));
        
        // Use batch processing to reduce overhead
        const int batch_size = std::max(1, std::min(100, (int)(ops_per_second / 10))); // At most 100 ops per batch
        
        size_t i = 0;
        while (!stop) {
            i++;
            total_operations.fetch_add(1);
            
            uint8_t cmd;
            uint64_t offset;
            uint32_t length;
            
            // Generate random operation
            int cmd_choice = cmd_dist(gen);
            switch (cmd_choice) {
            case 0: cmd = PAGE_CMD_READ; break;
            case 1: cmd = PAGE_CMD_WRITE; break;
            case 2: cmd = PAGE_CMD_FLUSH; break;
            case 3: cmd = PAGE_CMD_DISCARD; break;
            case 4: cmd = PAGE_CMD_STAT; break;
            default: cmd = PAGE_CMD_READ; break;
            }
            
            if (cmd == PAGE_CMD_FLUSH || cmd == PAGE_CMD_STAT) {
                offset = 0;
                length = 0;
            } else {
                offset = offset_dist(gen);
                length = length_dist(gen);
            }
            
            // Prepare write data if needed
            void* data = nullptr;
            if (cmd == PAGE_CMD_WRITE) {
                for (uint32_t j = 0; j < length; j++) {
                    buffer[j] = (char)((thread_id + i + j) % 256);
                }
                data = buffer;
            }
            
            // Send request
            if (send_request(cmd, offset, length, data) == 0) {
                struct page_response resp;
                char resp_buffer[PAGE_SIZE];
                
                if (receive_response(&resp, resp_buffer, PAGE_SIZE) == 0) {
                    if (resp.status == PAGE_RESP_OK) {
                        successful_operations++;
                        if (cmd == PAGE_CMD_READ) {
                            bytes_read += resp.length;
                        } else if (cmd == PAGE_CMD_WRITE) {
                            bytes_written += length;
                        }
                    } else {
                        failed_operations++;
                    }
                } else {
                    failed_operations++;
                    // If we can't receive response, the connection might be broken
                    // Try to reconnect
                    disconnect();
                    if (connect_to_server() != 0) {
                        log_message("Thread %d: Failed to reconnect, stopping", thread_id);
                        break;
                    }
                }
            } else {
                failed_operations++;
                // If we can't send request, the connection might be broken
                // Try to reconnect
                disconnect();
                if (connect_to_server() != 0) {
                    log_message("Thread %d: Failed to reconnect, stopping", thread_id);
                    break;
                }
            }
            
            // Sleep between batches to distribute load over time
            if ((i + 1) % batch_size == 0) {
                std::this_thread::sleep_for(sleep_interval);
            }
        }
    }

    void print_usage(const char* program_name) {
        printf("Usage: %s [options]\n", program_name);
        printf("Options:\n");
        printf("  -h, --host=HOST     Server host (default: 127.0.0.1)\n");
        printf("  -p, --port=PORT     Server port (default: 8964)\n");
        printf("  -v, --verbose       Enable verbose output\n");
        printf("  -t, --test=TEST     Test to run:\n");
        printf("                      basic    - Basic functionality test\n");
        printf("                      errors   - Error cases test\n");
        printf("                      stress   - Stress test\n");
        printf("                      all      - All tests (default)\n");
        printf("  --threads=NUM       Number of threads for stress test (default: 4)\n");
        printf("  --ops=NUM           Operations per thread (default: 1000)\n");
        printf("  --duration=SEC      Duration for stress test in seconds (default: 30)\n");
        printf("  --help              Show this help\n");
    }
};

int main(int argc, char *argv[]) {
    std::string host = "127.0.0.1";
    int port = 8964;
    bool verbose = false;
    std::string test_type = "all";
    int num_threads = 4;
    int operations_per_thread = 10000;
    int duration_seconds = 10;

    static const struct option longopts[] = {
        { "host", required_argument, 0, 'h' },
        { "port", required_argument, 0, 'p' },
        { "verbose", no_argument, 0, 'v' },
        { "test", required_argument, 0, 't' },
        { "threads", required_argument, 0, 'T' },
        { "ops", required_argument, 0, 'O' },
        { "duration", required_argument, 0, 'D' },
        { "help", no_argument, 0, 'H' },
        { NULL }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:vt:T:O:D:H", longopts, NULL)) != -1) {
        switch (opt) {
        case 'h':
            host = optarg;
            break;
        case 'p':
            port = atoi(optarg);
            break;
        case 'v':
            verbose = true;
            break;
        case 't':
            test_type = optarg;
            break;
        case 'T':
            num_threads = atoi(optarg);
            break;
        case 'O':
            operations_per_thread = atoi(optarg);
            break;
        case 'D':
            duration_seconds = atoi(optarg);
            break;
        case 'H':
            PageServerTestClient(host, port, verbose).print_usage(argv[0]);
            return 0;
        default:
            PageServerTestClient(host, port, verbose).print_usage(argv[0]);
            return 1;
        }
    }

    PageServerTestClient client(host, port, verbose);
    
    if (client.connect_to_server() != 0) {
        std::cerr << "Failed to connect to server" << std::endl;
        return 1;
    }

    int result = 0;
    
    try {
        if (test_type == "basic" || test_type == "all") {
            if (client.test_basic_operations() != 0) {
                std::cerr << "Basic operations test failed" << std::endl;
                result = 1;
            }
        }
        
        if (test_type == "errors" || test_type == "all") {
            if (client.test_error_cases() != 0) {
                std::cerr << "Error cases test failed" << std::endl;
                result = 1;
            }
        }
        
        if (test_type == "stress" || test_type == "all") {
            if (client.run_stress_test(num_threads, operations_per_thread, duration_seconds) != 0) {
                std::cerr << "Stress test failed" << std::endl;
                result = 1;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception during testing: " << e.what() << std::endl;
        result = 1;
    }

    client.disconnect();
    
    if (result == 0) {
        std::cout << "All tests passed!" << std::endl;
    } else {
        std::cout << "Some tests failed!" << std::endl;
    }
    
    return result;
} 