// SPDX-License-Identifier: MIT or GPL-2.0-only

#ifndef PAGESERVER_H
#define PAGESERVER_H

#include <stdint.h>
#include <sys/types.h>

#define PAGESERVER_MAGIC 0x50414745  // "PAGE"
#define PAGESERVER_VERSION 1

// Page server protocol commands
#define PAGE_CMD_READ    0x01
#define PAGE_CMD_WRITE   0x02
#define PAGE_CMD_FLUSH   0x03
#define PAGE_CMD_DISCARD 0x04
#define PAGE_CMD_STAT    0x05

// Page server response codes
#define PAGE_RESP_OK     0x00
#define PAGE_RESP_ERROR  0x01
#define PAGE_RESP_EOF    0x02

// Default page size (4KB)
#define PAGE_SIZE 4096

// Page server request header
struct page_request {
    uint32_t magic;      // PAGESERVER_MAGIC
    uint32_t version;    // PAGESERVER_VERSION
    uint8_t  cmd;        // Command type
    uint8_t  reserved[3]; // Reserved for alignment
    uint64_t offset;     // Byte offset
    uint32_t length;     // Length in bytes
    uint32_t reserved2;  // Reserved for alignment
} __attribute__((packed));

// Page server response header
struct page_response {
    uint32_t magic;      // PAGESERVER_MAGIC
    uint32_t version;    // PAGESERVER_VERSION
    uint8_t  status;     // Response status
    uint8_t  reserved[3]; // Reserved for alignment
    uint32_t length;     // Length of data following
    uint32_t reserved2;  // Reserved for alignment
} __attribute__((packed));

// Page server statistics
struct page_stats {
    uint64_t total_size;     // Total size in bytes
    uint32_t page_size;      // Page size in bytes
    uint32_t reserved;       // Reserved for alignment
} __attribute__((packed));

#endif // PAGESERVER_H 