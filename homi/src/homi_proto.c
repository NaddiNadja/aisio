#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <homi_proto.h>

static int
_read_from_buffer(int sock_fd, void *buf, size_t len)
{
	size_t done = 0;
	ssize_t n;

	while (done < len) {
		n = read(sock_fd, (char *)buf + done, len - done);
		if (n < 0) {
			return -errno;
		}
		if (n == 0) {
			return -EIO;
		}
		done += n;
	}

	return 0;
}

static int
_write_to_buffer(int sock_fd, void *buf, size_t len)
{
	size_t done = 0;
	ssize_t n;

	while (done < len) {
		n = write(sock_fd, (const char *)buf + done, len - done);
		if (n < 0) {
			return -errno;
		}
		done += n;
	}

	return 0;
}

int
homi_proto_socket_read(int sock_fd, struct homi_msg_header *hdr, void **buf,
		       int *fds, int max_fds, int *nfds_out)
{
	char cmsg_buf[CMSG_SPACE(HOMI_PROTO_MAX_FDS * sizeof(int))];
	struct msghdr msg = {0};
	struct cmsghdr *cmsg;
	struct iovec iov;
	char *payload = NULL;
	ssize_t n;
	int err;

	if (nfds_out) {
		*nfds_out = 0;
	}

	iov.iov_base = hdr;
	iov.iov_len = sizeof(*hdr);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsg_buf;
	msg.msg_controllen = sizeof(cmsg_buf);

	n = recvmsg(sock_fd, &msg, MSG_WAITALL);
	if (n < 0) {
		return -errno;
	}
	if ((size_t)n < sizeof(*hdr)) {
		return -EIO;
	}

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		int copy, nfds;
		int *recv_fds;

		if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
			continue;
		}

		nfds = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
		recv_fds = (int *)CMSG_DATA(cmsg);

		if (fds && max_fds > 0) {
			copy = nfds < max_fds ? nfds : max_fds;
			memcpy(fds, recv_fds, copy * sizeof(int));
			if (nfds_out) {
				*nfds_out = copy;
			}
			for (int i = copy; i < nfds; i++) {
				close(recv_fds[i]);
			}
		} else {
			for (int i = 0; i < nfds; i++) {
				close(recv_fds[i]);
			}
		}
		break;
	}

	if (hdr->payload_len > 0) {
		payload = malloc(hdr->payload_len);
		if (!payload) {
			return -errno;
		}

		err = _read_from_buffer(sock_fd, payload, hdr->payload_len);
		if (err) {
			free(payload);
			return err;
		}
	}

	*buf = payload;

	return 0;
}

int
homi_proto_socket_write(int sock_fd, struct homi_msg_header *hdr, void *buf, size_t buf_len,
			int *fds, int nfds)
{
	char cmsg_buf[CMSG_SPACE(HOMI_PROTO_MAX_FDS * sizeof(int))];
	struct msghdr msg = {0};
	struct cmsghdr *cmsg;
	struct iovec iov;
	ssize_t n;
	int err;

	hdr->payload_len = buf_len;

	iov.iov_base = hdr;
	iov.iov_len = sizeof(*hdr);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (fds && nfds > 0) {
		msg.msg_control = cmsg_buf;
		msg.msg_controllen = CMSG_SPACE(nfds * sizeof(int));
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(nfds * sizeof(int));
		memcpy(CMSG_DATA(cmsg), fds, nfds * sizeof(int));
	}

	n = sendmsg(sock_fd, &msg, 0);
	if (n < 0) {
		return -errno;
	}
	if ((size_t)n < sizeof(*hdr)) {
		return -EIO;
	}

	if (buf_len > 0) {
		err = _write_to_buffer(sock_fd, buf, buf_len);
		if (err) {
			return err;
		}
	}

	return 0;
}
