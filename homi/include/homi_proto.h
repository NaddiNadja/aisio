#ifndef HOMI_PROTO_H
#define HOMI_PROTO_H

#include <stdint.h>

#include <libxal.h>
#include <libxnvme_ipc.h>

#define HOMI_MAX_CONNECTS   8
#define HOMID_DEVURI_MAXLEN 256
#define HOMI_PROTO_MAX_FDS  3

enum homi_msg_type {
	HOMI_MSG_TYPE_XAL_CONNECT  = 1, ///< Request xal pool info for a device
	HOMI_MSG_TYPE_DEV_CONNECT  = 2, ///< Request a device metadata shell for a device
	HOMI_MSG_TYPE_QUEUE_CONNECT = 3, ///< Request a pre-provisioned queue pair for a device
};

struct homi_req_xal_connect {
	char dev_uri[HOMID_DEVURI_MAXLEN];
};

struct homi_res_xal_connect {
	int err;
	struct xal_sb sb;
	char shm_name[64];
};

struct homi_req_dev_connect {
	char dev_uri[HOMID_DEVURI_MAXLEN];
};

/* Note: xnvme_dev_ipc is ~18 KB; allocate homi_res_dev_connect on the heap. */
struct homi_res_dev_connect {
	int err;
	struct xnvme_dev_ipc ipc;
};

struct homi_req_queue_connect {
	char dev_uri[HOMID_DEVURI_MAXLEN];
	uint16_t capacity;
};

/* Note: allocate homi_res_queue_connect on the heap. */
struct homi_res_queue_connect {
	int err;
	struct xnvme_queue_ipc queue_ipc;
	struct xnvme_heap_ipc client_heap;
};

struct homi_msg_header {
	enum homi_msg_type type;
	size_t payload_len;
};

/**
 * Read a message from a socket.
 *
 * Reads a homi_msg_header followed by its payload from sock_fd. The payload
 * is heap-allocated and returned via *buf; the caller is responsible for
 * freeing it. *buf is set to NULL if payload_len is zero.
 *
 * If the message carries SCM_RIGHTS ancillary data, up to max_fds file
 * descriptors are stored in fds and the count is written to *nfds_out. Any
 * received file descriptors that exceed max_fds are closed immediately. Pass
 * fds=NULL / max_fds=0 if the caller does not expect file descriptors.
 *
 * @param sock_fd   File descriptor of the connected socket.
 * @param hdr       Output: populated with the received message header.
 * @param buf       Output: allocated buffer containing the payload, or NULL.
 * @param fds       Output: received file descriptors, or NULL.
 * @param max_fds   Capacity of fds; pass 0 if fds is NULL.
 * @param nfds_out  Output: number of file descriptors written to fds, or NULL.
 * @return          0 on success, negative errno on failure.
 */
int
homi_proto_socket_read(int sock_fd, struct homi_msg_header *hdr, void **buf,
		       int *fds, int max_fds, int *nfds_out);

/**
 * Write a message to a socket.
 *
 * Sends hdr followed by buf as a single framed message. Sets hdr->payload_len
 * to buf_len before writing. If nfds is non-zero, the file descriptors in fds
 * are attached as SCM_RIGHTS ancillary data on the header send. Pass
 * fds=NULL / nfds=0 to send without ancillary data.
 *
 * @param sock_fd  File descriptor of the connected socket.
 * @param hdr      Message header; payload_len will be overwritten with buf_len.
 * @param buf      Payload to send.
 * @param buf_len  Length of the payload in bytes.
 * @param fds      File descriptors to attach, or NULL.
 * @param nfds     Number of file descriptors in fds; pass 0 if fds is NULL.
 * @return         0 on success, negative errno on failure.
 */
int
homi_proto_socket_write(int sock_fd, struct homi_msg_header *hdr, void *buf, size_t buf_len,
			int *fds, int nfds);

#endif /* HOMI_PROTO_H */
