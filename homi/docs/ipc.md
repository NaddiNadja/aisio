# HOMI IPC Interface

The daemon (`homid`) exposes a Unix domain socket that clients use to request
access to pre-loaded xal trees. The socket path is set by `ipc_socket` in
`homi.conf` (default: `/run/homi/homi.sock`).

## Connection model

Each request uses its own connection. The client connects, sends one request,
reads one response, and closes the socket. The daemon spawns a thread per
accepted connection to handle the request.

## Message framing

Every message begins with a `homi_msg_header`:

```c
struct homi_msg_header {
    enum homi_msg_type type;
    size_t payload_len;
};
```

The header is followed immediately by `payload_len` bytes of payload. A
`payload_len` of zero means no payload follows.

`homi_proto_socket_write` and `homi_proto_socket_read` (declared in
`homi_proto.h`) implement this framing. The write function sets `payload_len`
before sending; the read function heap-allocates the payload and returns it via
an output pointer.

## Message types

### `HOMI_MSG_TYPE_HELLOWORLD` (0)

A test round-trip. The daemon echoes a fixed string back.

| Direction | Type | Description |
|-----------|------|-------------|
| Request   | `homi_req_helloworld` | `int32_t value` — ignored by the daemon |
| Response  | `char *` | Null-terminated string `"hello world!"` |

### `HOMI_MSG_TYPE_XAL_CONNECT` (1)

Requests the shared memory names for a device's xal pools.

| Direction | Type | Description |
|-----------|------|-------------|
| Request   | `homi_req_xal_connect` | `char dev_uri[256]` — device URI as configured in `homi.conf` |
| Response  | `homi_res_xal_connect` | `err` (negative errno on failure), `xal_sb` (superblock), `shm_name[64]` |

On success (`res.err == 0`), the client maps three POSIX shared memory segments
derived from `shm_name`:

| Segment | Name | Access | Contents |
|---------|------|--------|----------|
| Inodes | `{shm_name}_inodes` | read-only | xal inode pool |
| Extents | `{shm_name}_extents` | read-only | xal extent pool |
| Dirty flag | `{shm_name}_dirty` | read-only | `_Atomic bool`; `true` while the daemon is running `xal_index()` |

The client must not read from the inode or extent pools while the dirty flag is
`true`. See [client.md](client.md) for how to wait safely.

### `HOMI_MSG_TYPE_DEV_CONNECT` (2)

Requests a serialised snapshot of an open device's metadata (geometry,
identity, controller and namespace identify data). No file descriptors are
transferred.

| Direction | Type | Description |
|-----------|------|-------------|
| Request   | `homi_req_dev_connect` | `char dev_uri[256]` — device URI as configured in `homi.conf` |
| Response  | `homi_res_dev_connect` | `err` (negative errno on failure), `xnvme_dev_ipc` (flat metadata blob) |

On success (`res.err == 0`), the client passes `res.ipc` to `xnvme_dev_import()`
to construct a shell `xnvme_dev`. The shell provides geometry and namespace
information for command construction but cannot issue admin commands or
initialise queues.

### `HOMI_MSG_TYPE_QUEUE_CONNECT` (3)

Requests a pre-provisioned NVMe I/O queue pair. The daemon allocates a
dedicated SQ/CQ hugepage region, issues Create I/O CQ and Create I/O SQ admin
commands, and sends three file descriptors alongside the response via
`SCM_RIGHTS`.

| Direction | Type | Description |
|-----------|------|-------------|
| Request   | `homi_req_queue_connect` | `char dev_uri[256]`, `uint16_t capacity` — requested queue depth |
| Response  | `homi_res_queue_connect` | `err` (negative errno on failure), `xnvme_queue_ipc` (queue descriptor), `xnvme_heap_ipc client_heap` (physical address map for client DMA heap) |
| Ancillary fds | `SCM_RIGHTS[3]` | `queue_heap_fd` (SQ/CQ hugepage memfd), `bar_fd` (BAR0 memfd for doorbells), `client_heap_fd` (client DMA heap memfd) |

The three fds are only sent on success (`res.err == 0`). On failure no fds are
attached.

The client passes all three fds together with `res.queue_ipc` and
`res.client_heap` to `xnvme_queue_from_ipc()` to reconstruct the queue. All
fds are consumed by that call regardless of outcome.
