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
#include <errno.h>
#include "pageserver.h"

class PageServer {
private:
    int server_socket_fd_;
    int backing_file_fd_;
    std::string backing_file_;
    std::string listen_addr_;
    int listen_port_;
    bool verbose_;
    size_t file_size_;  // Size for file creation

    int handle_read_request(int client_fd, const struct page_request *req);
    int handle_write_request(int client_fd, const struct page_request *req);
    int handle_flush_request(int client_fd, const struct page_request *req);
    int handle_discard_request(int client_fd, const struct page_request *req);
    int handle_stat_request(int client_fd, const struct page_request *req);
    int send_response(int client_fd, uint8_t status, const void *data = nullptr, uint32_t data_len = 0);
    int receive_request(int client_fd, struct page_request *req);
    bool validate_request(const struct page_request *req);
    void log_message(const char *fmt, ...);
    int create_file_with_size(size_t size);

public:
    PageServer() : server_socket_fd_(-1), backing_file_fd_(-1), listen_port_(8964), verbose_(false), file_size_(0) {}
    ~PageServer() {
        if (server_socket_fd_ >= 0) close(server_socket_fd_);
        if (backing_file_fd_ >= 0) close(backing_file_fd_);
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
        { "size", required_argument, 0, 's' },
        { "verbose", no_argument, 0, 'v' },
        { "help", no_argument, 0, 'h' },
        { NULL }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "f:p:a:s:vh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'f':
            backing_file_ = optarg;
            break;
        case 'p':
            listen_port_ = atoi(optarg);
            break;
        case 'a':
            listen_addr_ = optarg;
            break;
        case 's':
            {
                char *endptr;
                file_size_ = strtoull(optarg, &endptr, 0);
                if (*endptr != '\0') {
                    // Handle suffixes: K, M, G
                    switch (*endptr) {
                    case 'K':
                    case 'k':
                        file_size_ *= 1024;
                        break;
                    case 'M':
                    case 'm':
                        file_size_ *= 1024 * 1024;
                        break;
                    case 'G':
                    case 'g':
                        file_size_ *= 1024 * 1024 * 1024;
                        break;
                    default:
                        std::cerr << "Error: invalid size suffix '" << *endptr << "'" << std::endl;
                        return -1;
                    }
                    if (*(endptr + 1) != '\0') {
                        std::cerr << "Error: invalid size format" << std::endl;
                        return -1;
                    }
                }
            }
            break;
        case 'v':
            verbose_ = true;
            break;
        case 'h':
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  -f, --file=FILE    Backing file path (required)\n"
                      << "  -p, --port=PORT    Listen port (default: 8964)\n"
                      << "  -a, --addr=ADDR    Listen address (default: 0.0.0.0)\n"
                      << "  -s, --size=SIZE    Create file with given size if file doesn't exist\n"
                      << "                     Size can be specified as: 1024, 1K, 1M, 1G, etc.\n"
                      << "  -v, --verbose      Enable verbose output\n"
                      << "  -h, --help         Show this help\n"
                      << "\n";
            return 1;
        default:
            return -1;
        }
    }

    if (backing_file_.empty()) {
        std::cerr << "Error: backing file path is required (-f/--file option)" << std::endl;
        return -1;
    }

    return 0;
}

int PageServer::init()
{
    // Check if file exists
    struct stat st;
    bool file_exists = (stat(backing_file_.c_str(), &st) == 0);
    
    if (file_exists && file_size_ > 0) {
        std::cerr << "Error: file '" << backing_file_ << "' already exists, cannot specify size" << std::endl;
        return -1;
    }
    
    if (!file_exists && file_size_ == 0) {
        std::cerr << "Error: file '" << backing_file_ << "' does not exist and no size specified" << std::endl;
        return -1;
    }

    // If file exists, set file_size_ to actual file size
    if (file_exists) {
        file_size_ = st.st_size;
    }

    // Open backing file
    backing_file_fd_ = open(backing_file_.c_str(), O_RDWR | O_CREAT, 0644);
    if (backing_file_fd_ < 0) {
        std::cerr << "Error: cannot open backing file " << backing_file_ << ": " << strerror(errno) << std::endl;
        return -1;
    }

    // Create file with specified size if requested (file doesn't exist)
    if (!file_exists && file_size_ > 0) {
        if (create_file_with_size(file_size_) != 0) {
            std::cerr << "Error: failed to create file with size " << file_size_ << std::endl;
            return -1;
        }
    }

    // Create server socket
    server_socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_fd_ < 0) {
        std::cerr << "Error: cannot create socket: " << strerror(errno) << std::endl;
        return -1;
    }

    // Set socket options
    int opt = 1;
    if (setsockopt(server_socket_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Error: setsockopt failed: " << strerror(errno) << std::endl;
        return -1;
    }

    // Bind socket
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listen_port_);
    if (listen_addr_.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        addr.sin_addr.s_addr = inet_addr(listen_addr_.c_str());
    }

    if (bind(server_socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Error: bind failed: " << strerror(errno) << std::endl;
        return -1;
    }

    // Listen for connections
    if (listen(server_socket_fd_, 5) < 0) {
        std::cerr << "Error: listen failed: " << strerror(errno) << std::endl;
        return -1;
    }

    if (file_exists) {
        log_message("Page server started on %s:%d, backing file: %s (existing, size: %zu bytes)", 
                    listen_addr_.empty() ? "0.0.0.0" : listen_addr_.c_str(), 
                    listen_port_, backing_file_.c_str(), file_size_);
    } else {
        log_message("Page server started on %s:%d, backing file: %s (created with size: %zu bytes)", 
                    listen_addr_.empty() ? "0.0.0.0" : listen_addr_.c_str(), 
                    listen_port_, backing_file_.c_str(), file_size_);
    }

    return 0;
}

int PageServer::run()
{
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_socket_fd_, (struct sockaddr*)&client_addr, &client_len);
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
                if (send_response(client_fd, PAGE_RESP_ERROR) < 0) {
                    log_message("Failed to send magic/version error response");
                    break;
                }
                log_message("Closing connection due to protocol error");
                break;
            }

            // Validate request parameters
            if (!validate_request(&req)) {
                if (send_response(client_fd, PAGE_RESP_ERROR) < 0) {
                    log_message("Failed to send validation error response");
                    break;
                }
                log_message("Closing connection due to validation error");
                break;
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
                if (send_response(client_fd, PAGE_RESP_ERROR) < 0) {
                    log_message("Failed to send invalid command error response");
                    break;
                }
                log_message("Closing connection due to invalid command");
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

bool PageServer::validate_request(const struct page_request *req)
{
    // For commands that use offset/length, validate bounds
    if (req->cmd == PAGE_CMD_READ || req->cmd == PAGE_CMD_WRITE || req->cmd == PAGE_CMD_DISCARD) {
        // Check for overflow in offset + length
        if (req->length > 0 && req->offset + req->length < req->offset) {
            log_message("Request offset + length overflow: %lu + %u", req->offset, req->length);
            return false;
        }
        
        // Check if request extends beyond file size
        if (req->offset + req->length > file_size_) {
            log_message("Request extends beyond file size: %lu + %u > %zu", 
                       req->offset, req->length, file_size_);
            return false;
        }
    }
    
    return true;
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
        if (ret < 0) {
            log_message("Failed to send response header: %s", strerror(errno));
        } else {
            log_message("Incomplete response header sent: %zd != %zu", ret, sizeof(resp));
        }
        return -1;
    }

    // Send data if any
    if (data && data_len > 0) {
        ret = send(client_fd, data, data_len, MSG_NOSIGNAL);
        if (ret != (ssize_t)data_len) {
            if (ret < 0) {
                log_message("Failed to send response data: %s", strerror(errno));
            } else {
                log_message("Incomplete response data sent: %zd != %u", ret, data_len);
            }
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
    ssize_t ret = pread(backing_file_fd_, buffer, req->length, req->offset);
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
    ret = pwrite(backing_file_fd_, buffer, req->length, req->offset);
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
    
    if (fsync(backing_file_fd_) < 0) {
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
    if (fallocate(backing_file_fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 
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
    if (fstat(backing_file_fd_, &st) < 0) {
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
    if (!verbose_) return;

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

int PageServer::create_file_with_size(size_t size)
{
    log_message("Creating file with size %zu bytes", size);
    
    if (size == 0) {
        log_message("File size is 0, creating empty file");
        return 0;
    }
    
    // Seek to the desired size - 1 and write a single byte
    // This will create a sparse file or allocate space depending on filesystem
    if (lseek(backing_file_fd_, size - 1, SEEK_SET) < 0) {
        std::cerr << "Error: lseek failed: " << strerror(errno) << std::endl;
        return -1;
    }
    
    char zero = 0;
    if (write(backing_file_fd_, &zero, 1) < 0) {
        std::cerr << "Error: write failed: " << strerror(errno) << std::endl;
        return -1;
    }
    
    // Ensure the file is actually written to disk
    if (fsync(backing_file_fd_) < 0) {
        std::cerr << "Error: fsync failed: " << strerror(errno) << std::endl;
        return -1;
    }
    
    log_message("File created successfully with size %zu bytes", size);
    return 0;
}

void PageServer::cleanup()
{
    if (server_socket_fd_ >= 0) {
        close(server_socket_fd_);
        server_socket_fd_ = -1;
    }
    if (backing_file_fd_ >= 0) {
        close(backing_file_fd_);
        backing_file_fd_ = -1;
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