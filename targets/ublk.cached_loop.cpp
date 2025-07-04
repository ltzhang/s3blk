// SPDX-License-Identifier: MIT or GPL-2.0-only

#include <config.h>

#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "ublksrv_tgt.h"
#include "cache/cache_manager.h"
#include "pageserver/pageserver.h"

struct cached_loop_tgt_data {
    bool user_copy;
    bool auto_zc;
    bool zero_copy;
    bool block_device;
    unsigned long offset;
    
    // Cache-related fields
    TemplateCacheManager<uint64_t, uint64_t, LRU> *cache;
    int remote_fd;                    // Connection to remote page server
    int cache_fd;                     // Cache file descriptor
    char remote_host[256];            // Remote server host
    int remote_port;                  // Remote server port
    pthread_t bg_thread;              // Background fetch thread
    bool bg_thread_running;           // Background thread status
    pthread_mutex_t bg_mutex;         // Mutex for background operations
    pthread_cond_t bg_cond;           // Condition for background operations
    
    // Background fetch queue
    struct fetch_queue_entry *fetch_queue;
    int fetch_queue_size;
    int fetch_queue_head;
    int fetch_queue_tail;
    pthread_mutex_t queue_mutex;
};

// Background fetch request structure
struct fetch_request {
    uint64_t logical_sector;
    uint64_t physical_sector;
    bool completed;
    int result;
};

struct fetch_queue_entry {
    uint64_t logical_sector;
    bool pending;
};

static bool backing_supports_discard(char *name)
{
    int fd;
    char buf[512];
    int len;

    len = snprintf(buf, 512, "/sys/block/%s/queue/discard_max_hw_bytes",
            basename(name));
    buf[len] = 0;
    fd = open(buf, O_RDONLY);
    if (fd > 0) {
        char val[128];
        int ret = pread(fd, val, 128, 0);
        unsigned long long bytes = 0;

        close(fd);
        if (ret > 0)
            bytes = strtol(val, NULL, 10);

        if (bytes > 0)
            return true;
    }
    return false;
}

static int connect_to_remote_server(const char *host, int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ublk_err("%s: cannot create socket: %s\n", __func__, strerror(errno));
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ublk_err("%s: cannot connect to %s:%d: %s\n", __func__, host, port, strerror(errno));
        close(sock);
        return -1;
    }
    
    return sock;
}

static int send_page_request(int fd, uint8_t cmd, uint64_t offset, uint32_t length)
{
    struct page_request req;
    req.magic = PAGESERVER_MAGIC;
    req.version = PAGESERVER_VERSION;
    req.cmd = cmd;
    req.offset = offset;
    req.length = length;
    
    ssize_t ret = send(fd, &req, sizeof(req), MSG_NOSIGNAL);
    if (ret != sizeof(req)) {
        ublk_err("%s: send failed: %s\n", __func__, strerror(errno));
        return -1;
    }
    
    return 0;
}

static int receive_page_response(int fd, struct page_response *resp)
{
    ssize_t ret = recv(fd, resp, sizeof(*resp), MSG_WAITALL);
    if (ret != sizeof(*resp)) {
        ublk_err("%s: recv failed: %s\n", __func__, strerror(errno));
        return -1;
    }
    
    if (resp->magic != PAGESERVER_MAGIC || resp->version != PAGESERVER_VERSION) {
        ublk_err("%s: invalid response magic/version\n", __func__);
        return -1;
    }
    
    return 0;
}

static int fetch_sector_from_remote(struct cached_loop_tgt_data *tgt_data, 
                                   uint64_t logical_sector, void *buffer)
{
    uint64_t offset = logical_sector << 9;  // Convert to bytes
    uint32_t length = 512;  // Sector size
    
    // Send read request
    if (send_page_request(tgt_data->remote_fd, PAGE_CMD_READ, offset, length) < 0) {
        return -1;
    }
    
    // Receive response
    struct page_response resp;
    if (receive_page_response(tgt_data->remote_fd, &resp) < 0) {
        return -1;
    }
    
    if (resp.status != PAGE_RESP_OK) {
        ublk_err("%s: remote read failed with status %d\n", __func__, resp.status);
        return -1;
    }
    
    // Receive data
    ssize_t ret = recv(tgt_data->remote_fd, buffer, resp.length, MSG_WAITALL);
    if (ret != (ssize_t)resp.length) {
        ublk_err("%s: recv data failed: %s\n", __func__, strerror(errno));
        return -1;
    }
    
    return 0;
}

static void *background_fetch_thread(void *arg)
{
    struct cached_loop_tgt_data *tgt_data = (struct cached_loop_tgt_data *)arg;
    
    while (tgt_data->bg_thread_running) {
        pthread_mutex_lock(&tgt_data->queue_mutex);
        
        // Wait for fetch requests
        while (tgt_data->fetch_queue_head == tgt_data->fetch_queue_tail && 
               tgt_data->bg_thread_running) {
            pthread_cond_wait(&tgt_data->bg_cond, &tgt_data->queue_mutex);
        }
        
        if (!tgt_data->bg_thread_running) {
            pthread_mutex_unlock(&tgt_data->queue_mutex);
            break;
        }
        
        // Get next fetch request
        int idx = tgt_data->fetch_queue_head;
        uint64_t logical_sector = tgt_data->fetch_queue[idx].logical_sector;
        tgt_data->fetch_queue[idx].pending = false;
        tgt_data->fetch_queue_head = (tgt_data->fetch_queue_head + 1) % tgt_data->fetch_queue_size;
        
        pthread_mutex_unlock(&tgt_data->queue_mutex);
        
        // Fetch from remote
        char buffer[512];
        int ret = fetch_sector_from_remote(tgt_data, logical_sector, buffer);
        
        if (ret == 0) {
            // Insert into cache - use logical_sector as both key and value for now
            uint64_t physical_sector = tgt_data->cache->insert(logical_sector, logical_sector);
            if (physical_sector != 0) {  // Check for valid return value
                // Write to cache file
                uint64_t cache_offset = physical_sector << 9;
                pwrite(tgt_data->cache_fd, buffer, 512, cache_offset);
            }
        }
    }
    
    return NULL;
}

static int cached_loop_setup_tgt(struct ublksrv_dev *dev, int type)
{
    struct ublksrv_tgt_info *tgt = &dev->tgt;
    const struct ublksrv_ctrl_dev *cdev = ublksrv_get_ctrl_dev(dev);
    const struct ublksrv_ctrl_dev_info *info = ublksrv_ctrl_get_dev_info(cdev);
    int fd, ret;
    unsigned long direct_io = 0;
    struct ublk_params p;
    char file[PATH_MAX];
    struct cached_loop_tgt_data *tgt_data = (struct cached_loop_tgt_data*)dev->tgt.tgt_data;
    struct stat sb;

    ret = ublk_json_read_target_str_info(cdev, "backing_file", file);
    if (ret < 0) {
        ublk_err("%s: backing file can't be retrieved from jbuf %d\n", __func__, ret);
        return ret;
    }

    ret = ublk_json_read_target_ulong_info(cdev, "direct_io", &direct_io);
    if (ret) {
        ublk_err("%s: read target direct_io failed %d\n", __func__, ret);
        return ret;
    }

    ret = ublk_json_read_target_ulong_info(cdev, "offset", &tgt_data->offset);
    if (ret) {
        ublk_err("%s: read target offset failed %d\n", __func__, ret);
        return ret;
    }

    ret = ublk_json_read_params(&p, cdev);
    if (ret) {
        ublk_err("%s: read ublk params failed %d\n", __func__, ret);
        return ret;
    }

    // Open cache file
    fd = open(file, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        ublk_err("%s: cache file %s can't be opened\n", __func__, file);
        return fd;
    }

    if (fstat(fd, &sb) < 0) {
        ublk_err("%s: unable to stat %s\n", __func__, file);
        return -1;
    }

    tgt_data->block_device = S_ISBLK(sb.st_mode);

    if (direct_io)
        fcntl(fd, F_SETFL, O_DIRECT);

    // Connect to remote server
    tgt_data->remote_fd = connect_to_remote_server(tgt_data->remote_host, tgt_data->remote_port);
    if (tgt_data->remote_fd < 0) {
        ublk_err("%s: cannot connect to remote server\n", __func__);
        close(fd);
        return -1;
    }

    // Initialize cache manager
    uint64_t cache_size_sectors = 1024;  // Default 512KB cache
    tgt_data->cache = new TemplateCacheManager<uint64_t, uint64_t, LRU>(cache_size_sectors);
    if (!tgt_data->cache) {
        ublk_err("%s: cannot create cache manager\n", __func__);
        close(tgt_data->remote_fd);
        close(fd);
        return -1;
    }

    // Initialize background fetch queue
    tgt_data->fetch_queue_size = 64;
    tgt_data->fetch_queue = (struct fetch_queue_entry *)calloc(tgt_data->fetch_queue_size, sizeof(*tgt_data->fetch_queue));
    if (!tgt_data->fetch_queue) {
        ublk_err("%s: cannot allocate fetch queue\n", __func__);
        delete tgt_data->cache;
        close(tgt_data->remote_fd);
        close(fd);
        return -1;
    }

    pthread_mutex_init(&tgt_data->queue_mutex, NULL);
    pthread_cond_init(&tgt_data->bg_cond, NULL);
    pthread_mutex_init(&tgt_data->bg_mutex, NULL);

    // Start background thread
    tgt_data->bg_thread_running = true;
    if (pthread_create(&tgt_data->bg_thread, NULL, background_fetch_thread, tgt_data) != 0) {
        ublk_err("%s: cannot create background thread\n", __func__);
        free(tgt_data->fetch_queue);
        delete tgt_data->cache;
        close(tgt_data->remote_fd);
        close(fd);
        return -1;
    }

    ublksrv_tgt_set_io_data_size(tgt);
    tgt->dev_size = p.basic.dev_sectors << 9;
    tgt->tgt_ring_depth = info->queue_depth;
    tgt->nr_fds = 1;
    tgt->fds[1] = fd;
    tgt_data->cache_fd = fd;

    tgt_data->auto_zc = info->flags & UBLK_F_AUTO_BUF_REG;
    tgt_data->zero_copy = info->flags & UBLK_F_SUPPORT_ZERO_COPY;
    tgt_data->user_copy = info->flags & UBLK_F_USER_COPY;
    if (tgt_data->zero_copy || tgt_data->user_copy)
        tgt->tgt_ring_depth *= 2;

    return 0;
}

static int cached_loop_recover_tgt(struct ublksrv_dev *dev, int type)
{
    dev->tgt.tgt_data = calloc(sizeof(struct cached_loop_tgt_data), 1);
    return cached_loop_setup_tgt(dev, type);
}

static int cached_loop_init_tgt(struct ublksrv_dev *dev, int type, int argc, char *argv[])
{
    const struct ublksrv_ctrl_dev *cdev = ublksrv_get_ctrl_dev(dev);
    const struct ublksrv_ctrl_dev_info *info = ublksrv_ctrl_get_dev_info(cdev);
    int buffered_io = 0;
    static const struct option lo_longopts[] = {
        { "file", 1, NULL, 'f' },
        { "buffered_io", no_argument, &buffered_io, 1},
        { "offset", required_argument, NULL, 'o'},
        { "remote_host", required_argument, NULL, 'h'},
        { "remote_port", required_argument, NULL, 'p'},
        { NULL }
    };
    unsigned long long bytes;
    struct stat st;
    int fd, opt;
    char *file = NULL;
    struct ublksrv_tgt_base_json tgt_json = { 0 };
    struct ublk_params p = {
        .types = UBLK_PARAM_TYPE_BASIC | UBLK_PARAM_TYPE_DISCARD | UBLK_PARAM_TYPE_DMA_ALIGN,
        .basic = {
            .attrs = UBLK_ATTR_VOLATILE_CACHE | UBLK_ATTR_FUA,
            .logical_bs_shift = 9,
            .physical_bs_shift = 12,
            .io_opt_shift = 12,
            .io_min_shift = 9,
            .max_sectors = info->max_io_buf_bytes >> 9,
        },
        .discard = {
            .max_discard_sectors = UINT_MAX >> 9,
            .max_discard_segments = 1,
        },
        .dma = {
            .alignment = 511,
        },
    };
    bool can_discard = false;
    unsigned long offset = 0;
    struct cached_loop_tgt_data *tgt_data;

    if (ublksrv_is_recovering(cdev))
        return cached_loop_recover_tgt(dev, 0);

    strcpy(tgt_json.name, "cached_loop");

    while ((opt = getopt_long(argc, argv, "-:f:o:h:p:", lo_longopts, NULL)) != -1) {
        switch (opt) {
        case 'f':
            file = strdup(optarg);
            break;
        case 'o':
            offset = strtoul(optarg, NULL, 10);
            break;
        case 'h':
            strncpy(tgt_data->remote_host, optarg, sizeof(tgt_data->remote_host) - 1);
            break;
        case 'p':
            tgt_data->remote_port = atoi(optarg);
            break;
        }
    }

    if (!file) {
        ublk_err("%s: cache file is required\n", __func__);
        return -1;
    }

    if (strlen(tgt_data->remote_host) == 0) {
        ublk_err("%s: remote host is required\n", __func__);
        return -1;
    }

    if (tgt_data->remote_port == 0) {
        tgt_data->remote_port = 8080;  // Default port
    }

    fd = open(file, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        ublk_err("%s: cache file %s can't be opened\n", __func__, file);
        return -2;
    }

    if (fstat(fd, &st) < 0)
        return -2;

    if (S_ISBLK(st.st_mode)) {
        unsigned int bs, pbs;

        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
            return -1;
        if (ioctl(fd, BLKSSZGET, &bs) != 0)
            return -1;
        if (ioctl(fd, BLKPBSZGET, &pbs) != 0)
            return -1;
        p.basic.logical_bs_shift = ilog2(bs);
        p.basic.physical_bs_shift = ilog2(pbs);
        can_discard = false; // backing_supports_discard(file);
    } else if (S_ISREG(st.st_mode)) {
        bytes = st.st_size;
        can_discard = true;
        p.basic.logical_bs_shift = ilog2(st.st_blksize);
        p.basic.physical_bs_shift = ilog2(st.st_blksize);
    } else {
        bytes = 0;
    }

    if (buffered_io || !ublk_param_is_valid(&p) || fcntl(fd, F_SETFL, O_DIRECT)) {
        p.basic.logical_bs_shift = 9;
        p.basic.physical_bs_shift = 12;
        buffered_io = 1;
    }

    if (bytes > 0) {
        unsigned long long offset_bytes = offset << 9;

        if (offset_bytes >= bytes) {
            ublk_err("%s: offset %lu greater than device size %llu", __func__, offset, bytes);
            return -2;
        }
        bytes -= offset_bytes;
    }

    tgt_json.dev_size = bytes;
    p.basic.dev_sectors = bytes >> 9;

    if (st.st_blksize && can_discard)
        p.discard.discard_granularity = st.st_blksize;
    else
        p.types &= ~UBLK_PARAM_TYPE_DISCARD;

    ublk_json_write_dev_info(cdev);
    ublk_json_write_target_base(cdev, &tgt_json);
    ublk_json_write_tgt_str(cdev, "backing_file", file);
    ublk_json_write_tgt_long(cdev, "direct_io", !buffered_io);
    ublk_json_write_tgt_ulong(cdev, "offset", offset);
    ublk_json_write_tgt_str(cdev, "remote_host", tgt_data->remote_host);
    ublk_json_write_tgt_long(cdev, "remote_port", tgt_data->remote_port);
    ublk_json_write_params(cdev, &p);

    close(fd);

    dev->tgt.tgt_data = calloc(sizeof(struct cached_loop_tgt_data), 1);
    tgt_data = (struct cached_loop_tgt_data*)dev->tgt.tgt_data;
    tgt_data->offset = offset;
    strncpy(tgt_data->remote_host, tgt_data->remote_host, sizeof(tgt_data->remote_host) - 1);
    tgt_data->remote_port = tgt_data->remote_port;

    return cached_loop_setup_tgt(dev, type);
}

static inline int cached_loop_fallocate_mode(const struct ublksrv_io_desc *iod)
{
       __u16 ublk_op = ublksrv_get_op(iod);
       __u32 flags = ublksrv_get_flags(iod);
       int mode = FALLOC_FL_KEEP_SIZE;

       /* follow logic of linux kernel loop */
       if (ublk_op == UBLK_IO_OP_DISCARD) {
               mode |= FALLOC_FL_PUNCH_HOLE;
       } else if (ublk_op == UBLK_IO_OP_WRITE_ZEROES) {
               if (flags & UBLK_IO_F_NOUNMAP)
                       mode |= FALLOC_FL_ZERO_RANGE;
               else
                       mode |= FALLOC_FL_PUNCH_HOLE;
       } else {
               mode |= FALLOC_FL_ZERO_RANGE;
       }

       return mode;
}

static inline void cached_loop_rw_handle_fua(struct io_uring_sqe *sqe,
		const struct ublksrv_io_desc *iod)
{
	if (ublksrv_get_op(iod) == UBLK_IO_OP_WRITE && (iod->op_flags & UBLK_IO_F_FUA))
		sqe->rw_flags |= RWF_DSYNC;
}

static int cached_loop_rw_user_copy(const struct ublksrv_queue *q,
		const struct ublksrv_io_desc *iod, int tag,
		const struct cached_loop_tgt_data *tgt_data)
{
	unsigned ublk_op = ublksrv_get_op(iod);
	struct io_uring_sqe *sqe[2];
	__u64 pos = ublk_pos(q->q_id, tag, 0);
	void *buf = ublksrv_queue_get_io_buf(q, tag);

	ublk_queue_alloc_sqes(q, sqe, 2);
	if (ublk_op == UBLK_IO_OP_READ) {
		/* read from cache file to io buffer */
		io_uring_prep_read(sqe[0], 1 /*fds[1]*/,
				buf,
				iod->nr_sectors << 9,
				pos);
		io_uring_sqe_set_flags(sqe[0], IOSQE_FIXED_FILE | IOSQE_IO_LINK);
		sqe[0]->user_data = build_user_data(tag, ublk_op, 0, 1);

		/* copy io buffer to ublkc device */
		io_uring_prep_write(sqe[1], 0 /*fds[0]*/,
				buf, iod->nr_sectors << 9, pos);
		io_uring_sqe_set_flags(sqe[1], IOSQE_FIXED_FILE);
		/* bit63 marks us as tgt io */
		sqe[1]->user_data = build_user_data(tag, UBLK_USER_COPY_WRITE, 0, 1);
	} else {
		/* copy ublkc device data to io buffer */
		io_uring_prep_read(sqe[0], 0 /*fds[0]*/,
			buf, iod->nr_sectors << 9, pos);
		io_uring_sqe_set_flags(sqe[0], IOSQE_FIXED_FILE | IOSQE_IO_LINK);
		sqe[0]->user_data = build_user_data(tag, UBLK_USER_COPY_READ, 0, 1);

		/* write data in io buffer to cache file */
		io_uring_prep_write(sqe[1], 1 /*fds[1]*/,
			buf, iod->nr_sectors << 9, pos);
		io_uring_sqe_set_flags(sqe[1], IOSQE_FIXED_FILE);
		cached_loop_rw_handle_fua(sqe[1], iod);
		/* bit63 marks us as tgt io */
		sqe[1]->user_data = build_user_data(tag, ublk_op, 0, 1);
	}
	return 2;
}

static int cached_loop_rw(const struct ublksrv_queue *q,
		const struct ublksrv_io_desc *iod, int tag,
		const struct cached_loop_tgt_data *tgt_data)
{
	enum io_uring_op uring_op = ublk_to_uring_fs_op(iod, tgt_data->auto_zc);
	void *buf = tgt_data->auto_zc ? NULL : (void *)iod->addr;
	struct io_uring_sqe *sqe[1];

	ublk_queue_alloc_sqes(q, sqe, 1);
	io_uring_prep_rw(uring_op,
		sqe[0],
		1 /*fds[1]*/,
		buf,
		iod->nr_sectors << 9,
		(iod->start_sector + tgt_data->offset) << 9);
	if (tgt_data->auto_zc)
		sqe[0]->buf_index = tag;

	io_uring_sqe_set_flags(sqe[0], IOSQE_FIXED_FILE);
	cached_loop_rw_handle_fua(sqe[0], iod);

	sqe[0]->user_data = build_user_data(tag, ublksrv_get_op(iod), 0, 1);
	return 1;
}

static int cached_loop_rw_zero_copy(const struct ublksrv_queue *q,
		const struct ublksrv_io_desc *iod, int tag,
		const struct cached_loop_tgt_data *tgt_data)
{
	unsigned ublk_op = ublksrv_get_op(iod);
	enum io_uring_op uring_op = ublk_to_uring_fs_op(iod, true);
	struct io_uring_sqe *sqe[3];

	ublk_queue_alloc_sqes(q, sqe, 3);

	io_uring_prep_buf_register(sqe[0], 0, tag, q->q_id, tag);
	sqe[0]->user_data = build_user_data(tag,
			ublk_cmd_op_nr(UBLK_U_IO_REGISTER_IO_BUF),
			0,
			1);
	sqe[0]->flags |= IOSQE_CQE_SKIP_SUCCESS | IOSQE_FIXED_FILE | IOSQE_IO_LINK;

	io_uring_prep_rw(uring_op,
			sqe[1],
			1 /*fds[1]*/,
			0,
			iod->nr_sectors << 9,
			(iod->start_sector + tgt_data->offset) << 9);
	sqe[1]->buf_index = tag;
	sqe[1]->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;
	sqe[1]->user_data = build_user_data(tag, ublk_op, 0, 1);

	io_uring_prep_buf_unregister(sqe[2], 0, tag, q->q_id, tag);
	sqe[2]->flags |= IOSQE_FIXED_FILE;
	sqe[2]->user_data = build_user_data(tag,
			ublk_cmd_op_nr(UBLK_U_IO_UNREGISTER_IO_BUF),
			0,
			1);

	// buf register is marked as IOSQE_CQE_SKIP_SUCCESS
	return 2;
}

static int cached_loop_queue_tgt_rw(const struct ublksrv_queue *q,
		const struct ublksrv_io_desc *iod, int tag,
		const struct cached_loop_tgt_data *data)
{
	/* auto_zc has top priority */
	if (data->auto_zc)
		return cached_loop_rw(q, iod, tag, data);
	if (data->zero_copy)
		return cached_loop_rw_zero_copy(q, iod, tag, data);
	if (data->user_copy)
		return cached_loop_rw_user_copy(q, iod, tag, data);
	return cached_loop_rw(q, iod, tag, data);
}

static int cached_loop_handle_flush(const struct ublksrv_queue *q,
		const struct ublksrv_io_desc *iod, int tag)
{
	struct io_uring_sqe *sqe[1];
	unsigned ublk_op = ublksrv_get_op(iod);

	ublk_queue_alloc_sqes(q, sqe, 1);
	io_uring_prep_fsync(sqe[0],
			1 /*fds[1]*/,
			IORING_FSYNC_DATASYNC);
	io_uring_sqe_set_flags(sqe[0], IOSQE_FIXED_FILE);
	/* bit63 marks us as tgt io */
	sqe[0]->user_data = build_user_data(tag, ublk_op, 0, 1);

	return 1;
}

static int cached_loop_handle_discard(const struct ublksrv_queue *q,
		const struct ublksrv_io_desc *iod, int tag,
		const struct cached_loop_tgt_data *data)
{
	struct io_uring_sqe *sqe[1];
	unsigned ublk_op = ublksrv_get_op(iod);

	ublk_queue_alloc_sqes(q, sqe, 1);
	io_uring_prep_fallocate(sqe[0], 1 /*fds[1]*/,
				cached_loop_fallocate_mode(iod),
				(iod->start_sector + data->offset) << 9,
				iod->nr_sectors << 9);
	io_uring_sqe_set_flags(sqe[0], IOSQE_FIXED_FILE);
	/* bit63 marks us as tgt io */
	sqe[0]->user_data = build_user_data(tag, ublk_op, 0, 1);
	return 1;
}

static int cached_loop_queue_tgt_io(const struct ublksrv_queue *q,
		const struct ublk_io_data *data, int tag)
{
	const struct ublksrv_io_desc *iod = data->iod;
	unsigned ublk_op = ublksrv_get_op(iod);
	const struct cached_loop_tgt_data *tgt_data = (struct cached_loop_tgt_data*) q->dev->tgt.tgt_data;
	int ret;

	switch (ublk_op) {
	case UBLK_IO_OP_FLUSH:
		ret = cached_loop_handle_flush(q, iod, tag);
		break;
	case UBLK_IO_OP_WRITE_ZEROES:
	case UBLK_IO_OP_DISCARD:
		ret = cached_loop_handle_discard(q, iod, tag, tgt_data);
		break;
	case UBLK_IO_OP_READ:
	case UBLK_IO_OP_WRITE:
		ret = cached_loop_queue_tgt_rw(q, iod, tag, tgt_data);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	ublk_dbg(UBLK_DBG_IO, "%s: tag %d ublk io %x %llx %u\n", __func__, tag,
			iod->op_flags, iod->start_sector, iod->nr_sectors << 9);
	return ret;
}

static co_io_job __cached_loop_handle_io_async(const struct ublksrv_queue *q,
		const struct ublk_io_data *data, int tag)
{
	// TODO: Implement proper async IO handling with cache
	// For now, just pass through to the basic IO handler
	int ret = cached_loop_queue_tgt_io(q, data, tag);
	
	if (ret > 0) {
		struct ublk_io_tgt *io = __ublk_get_io_tgt_data(data);
		int io_res = 0;
		while (ret-- > 0) {
			int res;
			co_await__suspend_always(tag);
			res = ublksrv_tgt_process_cqe(io, &io_res);
			if (res < 0 && io_res >= 0)
				io_res = res;
		}
		ublksrv_complete_io(q, tag, io_res);
	} else if (ret < 0) {
		ublk_err("fail to queue io %d, ret %d\n", tag, ret);
		ublksrv_complete_io(q, tag, ret);
	} else {
		ublk_err("no sqe %d\n", tag);
		ublksrv_complete_io(q, tag, -ENOMEM);
	}
}

static int cached_loop_handle_io_async(const struct ublksrv_queue *q,
		const struct ublk_io_data *data)
{
	struct ublk_io_tgt *io = __ublk_get_io_tgt_data(data);
	const struct cached_loop_tgt_data *tgt_data = (struct cached_loop_tgt_data*) q->dev->tgt.tgt_data;

	if (tgt_data->block_device && ublksrv_get_op(data->iod) == UBLK_IO_OP_DISCARD) {
		__u64 r[2];
		int res;

		io_uring_submit(q->ring_ptr);

		r[0] = (data->iod->start_sector + tgt_data->offset) << 9;
		r[1] = data->iod->nr_sectors << 9;
		res = ioctl(q->dev->tgt.fds[1], BLKDISCARD, &r);
		ublksrv_complete_io(q, data->tag, res);
	} else {
		io->co = __cached_loop_handle_io_async(q, data, data->tag);
	}
	return 0;
}

static void cached_loop_tgt_io_done(const struct ublksrv_queue *q,
		const struct ublk_io_data *data,
		const struct io_uring_cqe *cqe)
{
	ublksrv_tgt_io_done(q, data, cqe);
}

static void cached_loop_deinit_tgt(const struct ublksrv_dev *dev)
{
    struct cached_loop_tgt_data *tgt_data = (struct cached_loop_tgt_data*)dev->tgt.tgt_data;
    
    if (tgt_data) {
        // Stop background thread
        if (tgt_data->bg_thread_running) {
            tgt_data->bg_thread_running = false;
            pthread_cond_signal(&tgt_data->bg_cond);
            pthread_join(tgt_data->bg_thread, NULL);
        }
        
        // Cleanup
        if (tgt_data->cache) {
            delete tgt_data->cache;
        }
        
        if (tgt_data->fetch_queue) {
            free(tgt_data->fetch_queue);
        }
        
        if (tgt_data->remote_fd >= 0) {
            close(tgt_data->remote_fd);
        }
        
        pthread_mutex_destroy(&tgt_data->queue_mutex);
        pthread_cond_destroy(&tgt_data->bg_cond);
        pthread_mutex_destroy(&tgt_data->bg_mutex);
        
        fsync(dev->tgt.fds[1]);
        close(dev->tgt.fds[1]);
        free(tgt_data);
    }
}

static void cached_loop_cmd_usage()
{
    printf("\t-f cache_file --remote_host=HOST [--remote_port=PORT] [--buffered_io] [--offset NUM]\n");
    printf("\t\tcache_file is the local cache file\n");
    printf("\t\tremote_host is the page server host\n");
    printf("\t\tremote_port is the page server port (default: 8080)\n");
    printf("\t\toffset skips first NUM sectors on remote device\n");
}

static const struct ublksrv_tgt_type cached_loop_tgt_type = {
    .handle_io_async = cached_loop_handle_io_async,
    .tgt_io_done = cached_loop_tgt_io_done,
    .usage_for_add = cached_loop_cmd_usage,
    .init_tgt = cached_loop_init_tgt,
    .deinit_tgt = cached_loop_deinit_tgt,
    .name = "cached_loop",
};

int main(int argc, char *argv[])
{
    return ublksrv_main(&cached_loop_tgt_type, argc, argv);
} 