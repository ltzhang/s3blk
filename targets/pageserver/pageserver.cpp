// SPDX-License-Identifier: MIT or GPL-2.0-only

#include <config.h>
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
#include "pageserver.h"

class PageServer {
private:
    int server_socket_fd;
    int backing_file_fd;
    std::string backing_file;
    std::string listen_addr;
    int listen_port;
    bool verbose;

    int handle_read_request(int client_fd, const struct page_request *req);
    int handle_write_request(int client_fd, const struct page_request *req);
    int handle_flush_request(int client_fd, const struct page_request *req);
    int handle_discard_request(int client_fd, const struct page_request *req);
    int handle_stat_request(int client_fd, const struct page_request *req);
    int send_response(int client_fd, uint8_t status, const void *data = nullptr, uint32_t data_len = 0);
    int receive_request(int client_fd, struct page_request *req);
    void log_message(const char *fmt, ...);

public:
    PageServer() : server_socket_fd(-1), backing_file_fd(-1), listen_port(8080), verbose(false) {}
    ~PageServer() {
        if (server_socket_fd >= 0) close(server_socket_fd);
        if (backing_file_fd >= 0) close(backing_file_fd);
    }

    int parse_args(int argc, char *argv[]);
    int init();
    int run();
    void cleanup();
};

int PageServer::parse_args(int argc, char *argv[])
{
    static const struct option longopts[] = {
        { "file", required_argument, 0, 'f' },
        { "port", required_argument, 0, 'p' },
        { "addr", required_argument, 0, 'a' },
        { "verbose", no_argument, 0, 'v' },
        { "help", no_argument, 0, 'h' },
        { NULL }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "f:p:a:vh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'f':
            backing_file = optarg;
            break;
        case 'p':
            listen_port = atoi(optarg);
            break;
        case 'a':
            listen_addr = optarg;
            break;
        case 'v':
            verbose = true;
            break;
        case 'h':
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  -f, --file=FILE    Backing file path\n"
                      << "  -p, --port=PORT    Listen port (default: 8080)\n"
                      << "  -a, --addr=ADDR    Listen address (default: 0.0.0.0)\n"
                      << "  -v, --verbose      Enable verbose output\n"
                      << "  -h, --help         Show this help\n";
            return 1;
        default:
            return -1;
        }
    }

    if (backing_file.empty()) {
        std::cerr << "Error: backing file is required\n";
        return -1;
    }

    return 0;
}

int PageServer::init()
{
    // Open backing file
    backing_file_fd = open(backing_file.c_str(), O_RDWR | O_CREAT, 0644);
    if (backing_file_fd < 0) {
        std::cerr << "Error: cannot open backing file " << backing_file << ": " << strerror(errno) << std::endl;
        return -1;
    }

    // Create server socket
    server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_fd < 0) {
        std::cerr << "Error: cannot create socket: " << strerror(errno) << std::endl;
        return -1;
    }

    // Set socket options
    int opt = 1;
    if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Error: setsockopt failed: " << strerror(errno) << std::endl;
        return -1;
    }

    // Bind socket
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listen_port);
    if (listen_addr.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        addr.sin_addr.s_addr = inet_addr(listen_addr.c_str());
    }

    if (bind(server_socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Error: bind failed: " << strerror(errno) << std::endl;
        return -1;
    }

    // Listen for connections
    if (listen(server_socket_fd, 5) < 0) {
        std::cerr << "Error: listen failed: " << strerror(errno) << std::endl;
        return -1;
    }

    log_message("Page server started on %s:%d, backing file: %s", 
                listen_addr.empty() ? "0.0.0.0" : listen_addr.c_str(), 
                listen_port, backing_file.c_str());

    return 0;
}

int PageServer::run()
{
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_socket_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            std::cerr << "Error: accept failed: " << strerror(errno) << std::endl;
            continue;
        }

        log_message("Client connected from %s:%d", 
                    inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        // Handle client requests
        while (true) {
            struct page_request req;
            int ret = receive_request(client_fd, &req);
            if (ret < 0) {
                log_message("Client disconnected or error");
                break;
            }

            // Validate request
            if (req.magic != PAGESERVER_MAGIC || req.version != PAGESERVER_VERSION) {
                send_response(client_fd, PAGE_RESP_ERROR);
                continue;
            }

            // Handle request based on command
            switch (req.cmd) {
            case PAGE_CMD_READ:
                handle_read_request(client_fd, &req);
                break;
            case PAGE_CMD_WRITE:
                handle_write_request(client_fd, &req);
                break;
            case PAGE_CMD_FLUSH:
                handle_flush_request(client_fd, &req);
                break;
            case PAGE_CMD_DISCARD:
                handle_discard_request(client_fd, &req);
                break;
            case PAGE_CMD_STAT:
                handle_stat_request(client_fd, &req);
                break;
            default:
                send_response(client_fd, PAGE_RESP_ERROR);
                break;
            }
        }

        close(client_fd);
    }

    return 0;
}

int PageServer::receive_request(int client_fd, struct page_request *req)
{
    ssize_t ret = recv(client_fd, req, sizeof(*req), MSG_WAITALL);
    if (ret != sizeof(*req)) {
        return -1;
    }
    return 0;
}

int PageServer::send_response(int client_fd, uint8_t status, const void *data, uint32_t data_len)
{
    struct page_response resp;
    resp.magic = PAGESERVER_MAGIC;
    resp.version = PAGESERVER_VERSION;
    resp.status = status;
    resp.length = data_len;

    // Send response header
    ssize_t ret = send(client_fd, &resp, sizeof(resp), MSG_NOSIGNAL);
    if (ret != sizeof(resp)) {
        return -1;
    }

    // Send data if any
    if (data && data_len > 0) {
        ret = send(client_fd, data, data_len, MSG_NOSIGNAL);
        if (ret != (ssize_t)data_len) {
            return -1;
        }
    }

    return 0;
}

int PageServer::handle_read_request(int client_fd, const struct page_request *req)
{
    log_message("READ request: offset=%lu, length=%u", req->offset, req->length);

    // Allocate buffer for data
    char *buffer = new char[req->length];
    if (!buffer) {
        send_response(client_fd, PAGE_RESP_ERROR);
        return -1;
    }

    // Read from backing file
    ssize_t ret = pread(backing_file_fd, buffer, req->length, req->offset);
    if (ret < 0) {
        delete[] buffer;
        send_response(client_fd, PAGE_RESP_ERROR);
        return -1;
    }

    // Send response with data
    int status = (ret == 0) ? PAGE_RESP_EOF : PAGE_RESP_OK;
    send_response(client_fd, status, buffer, ret);

    delete[] buffer;
    return 0;
}

int PageServer::handle_write_request(int client_fd, const struct page_request *req)
{
    log_message("WRITE request: offset=%lu, length=%u", req->offset, req->length);

    // Allocate buffer for data
    char *buffer = new char[req->length];
    if (!buffer) {
        send_response(client_fd, PAGE_RESP_ERROR);
        return -1;
    }

    // Receive data from client
    ssize_t ret = recv(client_fd, buffer, req->length, MSG_WAITALL);
    if (ret != (ssize_t)req->length) {
        delete[] buffer;
        send_response(client_fd, PAGE_RESP_ERROR);
        return -1;
    }

    // Write to backing file
    ret = pwrite(backing_file_fd, buffer, req->length, req->offset);
    delete[] buffer;

    if (ret < 0) {
        send_response(client_fd, PAGE_RESP_ERROR);
        return -1;
    }

    send_response(client_fd, PAGE_RESP_OK);
    return 0;
}

int PageServer::handle_flush_request(int client_fd, const struct page_request *req)
{
    log_message("FLUSH request");
    
    if (fsync(backing_file_fd) < 0) {
        send_response(client_fd, PAGE_RESP_ERROR);
        return -1;
    }

    send_response(client_fd, PAGE_RESP_OK);
    return 0;
}

int PageServer::handle_discard_request(int client_fd, const struct page_request *req)
{
    log_message("DISCARD request: offset=%lu, length=%u", req->offset, req->length);

    // Use fallocate to punch holes
    if (fallocate(backing_file_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 
                  req->offset, req->length) < 0) {
        send_response(client_fd, PAGE_RESP_ERROR);
        return -1;
    }

    send_response(client_fd, PAGE_RESP_OK);
    return 0;
}

int PageServer::handle_stat_request(int client_fd, const struct page_request *req)
{
    log_message("STAT request");

    struct stat st;
    if (fstat(backing_file_fd, &st) < 0) {
        send_response(client_fd, PAGE_RESP_ERROR);
        return -1;
    }

    struct page_stats stats;
    stats.total_size = st.st_size;
    stats.page_size = PAGE_SIZE;

    send_response(client_fd, PAGE_RESP_OK, &stats, sizeof(stats));
    return 0;
}

void PageServer::log_message(const char *fmt, ...)
{
    if (!verbose) return;

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

void PageServer::cleanup()
{
    if (server_socket_fd >= 0) {
        close(server_socket_fd);
        server_socket_fd = -1;
    }
    if (backing_file_fd >= 0) {
        close(backing_file_fd);
        backing_file_fd = -1;
    }
}

int main(int argc, char *argv[])
{
    PageServer server;
    
    if (server.parse_args(argc, argv) != 0) {
        return 1;
    }

    if (server.init() != 0) {
        return 1;
    }

    server.run();
    server.cleanup();
    
    return 0;
} 