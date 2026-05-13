#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <homid.h>
#include <homid_ipc.h>
#include <homid_log.h>
#include <homid_xal.h>
#include <homi_proto.h>

struct worker_args {
	int client_fd;
	struct homid *homid;
};

static int
_open_socket(char *socket_path)
{
	struct sockaddr_un saddr;
	int fd, err = 0;

	fd = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (fd < 0) {
		homid_log(LOG_ERR, "Failed: socket(); err(%d)", errno);
		return -errno;
	}

	saddr.sun_family = AF_LOCAL;
	strncpy(saddr.sun_path, socket_path, sizeof(saddr.sun_path));
	saddr.sun_path[sizeof(saddr.sun_path) - 1] = '\0';

	err = bind(fd, (struct sockaddr *)&saddr, sizeof(saddr));
	if (err) {
		homid_log(LOG_ERR, "Failed: bind(); err(%d)", errno);
		close(fd);
		return -errno;
	}

	return fd;
}

int
homid_ipc_open(char *socket_path, struct homid_ipc_connection **conn)
{
	struct homid_ipc_connection *cand;
	int fd, err = 0;

	cand = calloc(1, sizeof(*cand));
	if (!cand) {
		err = -errno;
		homid_log(LOG_CRIT, "Failed: calloc(); errno(%d)", err);
		return err;
	}

	fd = _open_socket(socket_path);
	if (fd < 0) {
		err = fd;
		homid_log(LOG_ERR, "Failed: _open_socket(); err(%d)", err);
		goto failed;
	}
	cand->fd = fd;

	err = listen(fd, HOMI_MAX_CONNECTS);
	if (err) {
		err = -errno;
		homid_log(LOG_ERR, "Failed: listen(); err(%d)", err);
		goto failed;
	}

	homid_log(LOG_INFO, "Listening for client connections ...");

	*conn = cand;

	return 0;

failed:
	homid_ipc_close(cand);

	return err;
}

void
homid_ipc_close(struct homid_ipc_connection *conn)
{
	if (!conn) {
		homid_log(LOG_INFO, "No homid_ipc_connection given; skipping homid_ipc_close()");
		return;
	}

	if (conn->fd >= 0) {
		close(conn->fd);
	}
}

static void *
worker(void *arg)
{
	struct worker_args *wargs = arg;
	struct homid *homid = wargs->homid;
	int sock_fd = wargs->client_fd;
	struct homi_msg_header hdr;
	void *payload;
	int err;

	free(wargs);

	err = homi_proto_socket_read(sock_fd, &hdr, &payload, NULL, 0, NULL);
	if (err) {
		homid_log(LOG_ERR, "Failed: homi_proto_socket_read(hdr); err(%d)", err);
		goto exit;
	}

	switch ((enum homi_msg_type)hdr.type) {
	case HOMI_MSG_TYPE_XAL_CONNECT:
		struct homi_req_xal_connect *req = (struct homi_req_xal_connect *)payload;
		struct homi_res_xal_connect res = {0};
		struct homid_device *device = NULL;

		if (!req) {
			homid_log(LOG_ERR, "Error: Payload required for XAL_CONNECT request");
			res.err = -EINVAL;
			goto send_response;
		}

		device = homid_device_get(homid, req->dev_uri);

		if (!device) {
			homid_log(LOG_ERR, "XAL_CONNECT: device not found: %s", req->dev_uri);
			res.err = -ENODEV;
			goto send_response;
		}

		res.sb = *xal_get_sb(device->xal);
		memcpy(res.shm_name, device->shm_name, sizeof(res.shm_name));

send_response:
		err = homi_proto_socket_write(sock_fd, &hdr, &res, sizeof(res), NULL, 0);
		if (err) {
			homid_log(LOG_ERR, "Failed: homi_proto_socket_write(); err(%d)", err);
		}

		break;

	case HOMI_MSG_TYPE_DEV_CONNECT:
		struct homi_req_dev_connect *dev_req = (struct homi_req_dev_connect *)payload;
		struct homi_res_dev_connect *dev_res;
		struct homid_device *dev_device;

		dev_res = calloc(1, sizeof(*dev_res));
		if (!dev_res) {
			homid_log(LOG_CRIT, "Failed: calloc() for DEV_CONNECT res");
			goto exit;
		}

		if (!dev_req) {
			homid_log(LOG_ERR, "Error: Payload required for DEV_CONNECT request");
			dev_res->err = -EINVAL;
			goto send_dev_res;
		}

		dev_device = homid_device_get(homid, dev_req->dev_uri);
		if (!dev_device) {
			homid_log(LOG_ERR, "DEV_CONNECT: device not found: %s", dev_req->dev_uri);
			dev_res->err = -ENODEV;
			goto send_dev_res;
		}

		err = xnvme_dev_export(dev_device->dev, &dev_res->ipc);
		if (err) {
			homid_log(LOG_ERR, "DEV_CONNECT: xnvme_dev_export(); err(%d)", err);
			dev_res->err = err;
		}

send_dev_res:
		err = homi_proto_socket_write(sock_fd, &hdr, dev_res, sizeof(*dev_res), NULL, 0);
		if (err) {
			homid_log(LOG_ERR, "Failed: homi_proto_socket_write(); err(%d)", err);
		}

		free(dev_res);
		break;

	case HOMI_MSG_TYPE_QUEUE_CONNECT:
		struct homi_req_queue_connect *q_req = (struct homi_req_queue_connect *)payload;
		struct homi_res_queue_connect *q_res;
		struct homid_device *q_device;
		struct xnvme_queue *q_queue;
		struct homid_provisioned_queue *pq;
		void *client_heap;
		int out_fds[HOMI_PROTO_MAX_FDS];
		int queue_heap_fd, bar_fd, client_heap_fd;

		q_res = calloc(1, sizeof(*q_res));
		if (!q_res) {
			homid_log(LOG_CRIT, "Failed: calloc() for QUEUE_CONNECT res");
			goto exit;
		}

		if (!q_req) {
			homid_log(LOG_ERR, "Error: Payload required for QUEUE_CONNECT request");
			q_res->err = -EINVAL;
			goto send_q_res;
		}

		q_device = homid_device_get(homid, q_req->dev_uri);
		if (!q_device) {
			homid_log(LOG_ERR, "QUEUE_CONNECT: device not found: %s", q_req->dev_uri);
			q_res->err = -ENODEV;
			goto send_q_res;
		}

		err = xnvme_queue_init_shared(q_device->dev, q_req->capacity, 0, &q_queue,
					      &q_res->queue_ipc, &queue_heap_fd, &bar_fd);
		if (err) {
			homid_log(LOG_ERR, "QUEUE_CONNECT: xnvme_queue_init_shared(); err(%d)", err);
			q_res->err = err;
			goto send_q_res;
		}

		err = xnvme_client_heap_alloc(&q_res->client_heap, &client_heap_fd, &client_heap);
		if (err) {
			homid_log(LOG_ERR, "QUEUE_CONNECT: xnvme_client_heap_alloc(); err(%d)", err);
			q_res->err = err;
			xnvme_queue_term(q_queue);
			close(queue_heap_fd);
			close(bar_fd);
			goto send_q_res;
		}

		pq = realloc(q_device->queues, (q_device->nqueues + 1) * sizeof(*pq));
		if (!pq) {
			q_res->err = -ENOMEM;
			xnvme_client_heap_free(client_heap);
			close(client_heap_fd);
			xnvme_queue_term(q_queue);
			close(queue_heap_fd);
			close(bar_fd);
			goto send_q_res;
		}

		pq[q_device->nqueues].queue = q_queue;
		pq[q_device->nqueues].client_heap = client_heap;
		pq[q_device->nqueues].queue_heap_fd = queue_heap_fd;
		pq[q_device->nqueues].bar_fd = bar_fd;
		pq[q_device->nqueues].client_heap_fd = client_heap_fd;
		q_device->queues = pq;
		q_device->nqueues++;

		out_fds[0] = queue_heap_fd;
		out_fds[1] = bar_fd;
		out_fds[2] = client_heap_fd;

send_q_res:
		if (q_res->err) {
			err = homi_proto_socket_write(sock_fd, &hdr, q_res, sizeof(*q_res), NULL, 0);
		} else {
			err = homi_proto_socket_write(sock_fd, &hdr, q_res, sizeof(*q_res),
						      out_fds, HOMI_PROTO_MAX_FDS);
		}
		if (err) {
			homid_log(LOG_ERR, "Failed: homi_proto_socket_write(); err(%d)", err);
		}

		free(q_res);
		break;

	default:
		homid_log(LOG_WARNING, "Unknown message type: %u", hdr.type);
		break;
	}

exit:
	free(payload);
	close(sock_fd);
	return NULL;
}

int
homid_ipc_accept(struct homid *homid)
{
	struct homid_ipc_connection *conn;
	struct worker_args *wargs;
	struct sockaddr addr;
	pthread_t thr_id;
	uint32_t len;
	int client_fd, err;

	if (!homid) {
		homid_log(LOG_ERR, "Error: No homid struct given");
		return -EINVAL;
	}

	conn = homid->conn;

	len = sizeof(addr);

	homid_log(LOG_DEBUG, "Waiting for incoming connections...\n");
	client_fd = accept(conn->fd, &addr, &len);

	if (client_fd < 0) {
		homid_log(LOG_WARNING, "Failed: accept(); continuing");
		return 0;
	}

	wargs = calloc(1, sizeof(*wargs));
	if (!wargs) {
		err = -errno;
		homid_log(LOG_CRIT, "Failed: calloc(); err(%d)", err);
		close(client_fd);
		return err;
	}

	wargs->client_fd = client_fd;
	wargs->homid = homid;

	err = pthread_create(&thr_id, NULL, worker, wargs);
	if (err) {
		homid_log(LOG_ERR, "Failed: pthread_create(); err(%d)", err);
		free(wargs);
		close(client_fd);
		return err;
	}

	return 0;
}
