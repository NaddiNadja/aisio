# Plan: HOMI queue-pair serving

## Goal

Enable user space clients to perform I/O directly on a uPCIe-managed NVMe
device without opening the device themselves. The daemon is the sole owner of
the controller; clients receive a pre-provisioned queue pair via IPC and submit
NVMe commands on it without daemon involvement on the data path.

## Architecture

A client needs two things:

1. **Device metadata** — geometry (`geo`) and identity (`ident`, `nsid`) needed
   to construct NVMe commands. Serialisable as a flat blob; no controller access
   needed.
2. **A queue pair** — SQ and CQ registered with the controller, plus doorbell
   access. The daemon provisions this on behalf of the client and shares it via
   file descriptor passing.

After setup, the client holds a shell `xnvme_dev` (metadata only, no backend)
and an `xnvme_queue` backed by memory it shares with the daemon. I/O proceeds
without any daemon involvement.

## Required changes

### 1. xNVMe: `xnvme_dev_export` and `xnvme_dev_import`

Create `xnvme/lib/xnvme_ipc.c`; the header `libxnvme_ipc.h` already declares
`xnvme_dev_ipc`, `xnvme_dev_export`, and `xnvme_dev_import`.

**`xnvme_dev_export`** serialises an open `xnvme_dev` into a flat, copyable
`xnvme_dev_ipc` blob containing geo, ident, id, and idcss. No fds, no backend
state. Called by the daemon.

**`xnvme_dev_import`** constructs a shell `xnvme_dev` from such a blob. It does
not call `xnvme_dev_open` and does not touch the controller. The shell has
geo/ident populated and is sufficient for command construction (`ssw`, `nsid`).
Admin commands and `xnvme_queue_init` on the shell will fail; I/O is only
possible via an externally provisioned queue. The docstring in the header
describes the old behaviour (opens the device); update it to match.

`xnvme_dev_close` on the shell must not invoke backend teardown — guard the
`ctrlr_term` call against null `be.state`. After `xnvme_queue_from_ipc` runs,
`shell_dev->be.dev.dev_close` is set to `xnvme_be_nosys_dev_close` (non-NULL),
so `xnvme_dev_close` enters the teardown block and reads
`((void **)dev->be.state)[0]` — but `be.state` is NULL on a shell device,
causing a null-pointer dereference. Fix in `lib/xnvme_dev.c`:

```c
if (dev->be.dev.dev_close) {
    void *ctrlr = dev->be.state ? ((void **)dev->be.state)[0] : NULL;

    dev->be.dev.dev_close(dev);

    if (dev->be.dev.ctrlr_term && ctrlr) {
        xnvme_be_cref_deref(ctrlr, XNVME_BE_CREF_DESTROY_IMMEDIATE);
    }
}
```

### 2. xNVMe: `struct xnvme_queue_ipc` and `struct xnvme_heap_ipc`

New structs in `libxnvme_ipc.h` describing shared resources:

```c
struct xnvme_heap_ipc {
    size_t   size;           /* total size of the hugepage mapping */
    uint32_t nphys;          /* number of backing hugepages */
    uint64_t phys_lut[64];  /* one physical address per 2 MB hugepage */
};

struct xnvme_queue_ipc {
    uint32_t qid;
    uint16_t depth;
    uint16_t tail;           /* initial SQ tail */
    uint16_t head;           /* initial CQ head */
    uint8_t  phase;          /* initial CQ phase bit */
    uint64_t sq_heap_offset; /* byte offset of SQ within the queue heap fd */
    uint64_t cq_heap_offset; /* byte offset of CQ within the queue heap fd */
    struct xnvme_heap_ipc queue_heap;
    uint64_t sqdb_bar_offset;
    uint64_t cqdb_bar_offset;
};
```

### 3. xNVMe: `xnvme_queue_init_shared` and `xnvme_queue_from_ipc`

**`xnvme_queue_init_shared`** — called by the daemon. Allocates SQ and CQ from
a dedicated `memfd_create(MFD_HUGETLB)` rather than the global
`g_upcie_rte.heap`. Issues Create I/O CQ and Create I/O SQ admin commands via
the daemon's controller. Returns the queue, the serialised descriptor, and two
fds:

```c
int xnvme_queue_init_shared(struct xnvme_dev *dev,
                             uint16_t capacity,
                             int opts,
                             struct xnvme_queue **out_queue,
                             struct xnvme_queue_ipc *out_ipc,
                             int *out_queue_heap_fd,
                             int *out_bar_fd);
```

`out_queue_heap_fd` is the `memfd` holding the SQ/CQ hugepages.
`out_bar_fd` is:
- sysfs backend: `open("/sys/bus/pci/devices/{bdf}/resource0", O_RDWR)`
- vfio backend: dup of the vfio device fd with the BAR already mapped

Both fds remain open in the daemon for the lifetime of the queue.

**`xnvme_queue_from_ipc`** — called by the client. Maps the received fds,
allocates a process-local request pool (CID tracking is per-process), and
constructs the queue:

```c
int xnvme_queue_from_ipc(struct xnvme_dev *shell_dev,
                          const struct xnvme_queue_ipc *ipc,
                          const struct xnvme_heap_ipc *client_heap_ipc,
                          int queue_heap_fd,
                          int bar_fd,
                          int client_heap_fd,
                          struct xnvme_queue **out);
```

The `client_heap_ipc` carries the pre-resolved physical address map so the client
can call `hostmem_heap_import` without CAP_SYS_ADMIN.

Internally:
1. `mmap(queue_heap_fd, MAP_SHARED)` → compute `sq` and `cq` virtual addresses
   from `ipc->sq_heap_offset` / `ipc->cq_heap_offset`.
2. `mmap(bar_fd, MAP_SHARED)` → compute `sqdb` and `cqdb` from
   `ipc->sqdb_bar_offset` / `ipc->cqdb_bar_offset`.
3. Initialise `g_upcie_rte.heap` from `client_heap_fd` using
   `hostmem_heap_import` (see below) so the client can allocate PRP pages and
   I/O buffers without CAP_SYS_ADMIN.
4. Allocate `nvme_request_pool` via calloc; initialise with
   `nvme_request_pool_init` and `nvme_request_pool_init_prps` using
   `g_upcie_rte.heap`.
5. Fill `nvme_qpair` with the mapped pointers and initial tail/head/phase.
6. Allocate `xnvme_queue_upcie`; set `base.dev = shell_dev`.
7. Set the uPCIe async backend function pointers (`cmd_io`, `poke`, `term`) on
   `shell_dev->be` so `xnvme_nvm_read` / `xnvme_nvm_write` dispatch correctly.

`xnvme_queue_term` on the client side unmaps the SQ/CQ heap and BAR, frees the
rpool, and closes the fds.

**`xnvme_client_heap_alloc`** — called by the daemon to allocate a one-hugepage
DMA heap for the client. Allocates the heap via `hostmem_heap_init`, populates
an `xnvme_heap_ipc` with the physical address map, and returns a dup of the
underlying memfd. The client receives this fd via SCM_RIGHTS and maps it with
`hostmem_heap_import` inside `xnvme_queue_from_ipc`. The opaque heap pointer
must be kept by the daemon and passed to `xnvme_client_heap_free` at cleanup.

```c
int  xnvme_client_heap_alloc(struct xnvme_heap_ipc *out_ipc, int *out_fd, void **out_opaque);
void xnvme_client_heap_free(void *opaque);
```

### 4. hostmem: `hostmem_hugepage_alloc_fd` and `hostmem_heap_import`

Two new functions needed in the upcie hostmem layer:

**`hostmem_hugepage_alloc_fd`** — like `hostmem_hugepage_alloc` but creates the
hugepage via `memfd_create(name, MFD_HUGETLB)` and returns the fd alongside the
mapping. The existing `HOSTMEM_BACKEND_MEMFD` path already uses `memfd_create`;
this exposes the fd to callers.

**`hostmem_heap_import`** — initialises a `hostmem_heap` from a caller-provided
memfd and a pre-computed `phys_lut` (from `xnvme_heap_ipc`). Used by the client
to set up its DMA heap without reading `/proc/self/pagemap` (the daemon has
already resolved the physical addresses and embedded them in the IPC struct).

### xNVMe: build system wiring

Three files need updating so that `xnvme_ipc.c` is compiled, its header is
installed, and its symbols are exported.

**`lib/meson.build`** — add `'xnvme_ipc.c'` to `xnvmelib_source`.

**`include/meson.build`** — add `install_headers('libxnvme_ipc.h')`.

**`lib/libxnvme.map`** — add the new symbols to the exported set:

```
xnvme_dev_export;
xnvme_dev_import;
xnvme_queue_init_shared;
xnvme_queue_from_ipc;
xnvme_client_heap_alloc;
xnvme_client_heap_free;
```

### 5. HOMI IPC: update framing to support fd passing

`homi_proto_socket_write` and `homi_proto_socket_read` currently use plain
`write` / `read`. Replace them with `sendmsg` / `recvmsg` so that ancillary
`SCM_RIGHTS` data can optionally be attached. Callers that pass `NULL` / `0` for
the fd parameters behave identically to before.

Updated signatures:

```c
int homi_proto_socket_write(int sock_fd,
                             struct homi_msg_header *hdr,
                             void *buf, size_t buf_len,
                             int *fds, int nfds);

int homi_proto_socket_read(int sock_fd,
                            struct homi_msg_header *hdr,
                            void **buf,
                            int *fds, int max_fds,
                            int *nfds_out);
```

### 6. HOMI protocol: `HOMI_MSG_TYPE_DEV_CONNECT` and `HOMI_MSG_TYPE_QUEUE_CONNECT`

Two new entries in `homi_proto.h`:

```c
HOMI_MSG_TYPE_DEV_CONNECT   = 2,
HOMI_MSG_TYPE_QUEUE_CONNECT = 3,
```

**`DEV_CONNECT`** — returns a serialised device metadata blob. No fds.

```c
struct homi_req_dev_connect {
    char dev_uri[HOMID_DEVURI_MAXLEN];
};

struct homi_res_dev_connect {
    int err;
    struct xnvme_dev_ipc ipc;
};
```

**`QUEUE_CONNECT`** — returns a queue descriptor plus three fds via SCM_RIGHTS.

```c
struct homi_req_queue_connect {
    char     dev_uri[HOMID_DEVURI_MAXLEN];
    uint16_t capacity;
};

struct homi_res_queue_connect {
    int err;
    struct xnvme_queue_ipc queue_ipc;
    struct xnvme_heap_ipc  client_heap;
};
```

Three fds sent alongside the response:
- `queue_heap_fd` — hugepage memfd for SQ/CQ
- `bar_fd` — BAR0 fd for doorbell access
- `client_heap_fd` — small hugepage memfd for the client's PRP pages and I/O
  buffers (daemon allocates, e.g., 64 MB; provides `phys_lut` in `client_heap`)

### 7. Daemon: new cases in `homid_ipc.c`

**`DEV_CONNECT`** case in `worker()`:

1. Heap-allocate `homi_res_dev_connect`.
2. Look up device via `homid_device_get`.
3. Call `xnvme_dev_export(device->dev, &res->ipc)`.
4. Send response via `homi_proto_socket_write` (no fds).

**`QUEUE_CONNECT`** case in `worker()`:

1. Heap-allocate `homi_res_queue_connect`.
2. Look up device via `homid_device_get`.
3. Call `xnvme_queue_init_shared(...)` → queue, queue_ipc, queue_heap_fd, bar_fd.
4. Allocate a small hugepage memfd for the client heap; resolve its `phys_lut`;
   embed in `res->client_heap`.
5. Store the provisioned queue in a per-device queue list for cleanup on shutdown.
6. Call `homi_proto_socket_write(sock_fd, &hdr, res, sizeof(*res), fds, 3)`.

### 8. libhomic: `homic_dev_connect` and `homic_queue_connect`

**`homic_dev_connect`**:

```c
int homic_dev_connect(char *dev_uri, struct xnvme_dev **out);
```

1. Connect, send `DEV_CONNECT`, receive `homi_res_dev_connect`.
2. Call `xnvme_dev_import(&res->ipc)` → returns shell `xnvme_dev`.

**`homic_queue_connect`**:

```c
int homic_queue_connect(char *dev_uri, uint16_t capacity,
                        struct xnvme_dev *shell_dev,
                        struct xnvme_queue **out);
```

1. Connect, send `QUEUE_CONNECT`, receive response + three fds.
2. Call `xnvme_queue_from_ipc(shell_dev, &res->queue_ipc, &res->client_heap,
   queue_heap_fd, bar_fd, client_heap_fd, out)`.

## Data flow

```
Client                              Daemon (worker thread)
──────                              ──────────────────────
homic_dev_connect(uri)
  send DEV_CONNECT {uri}       ───▶  xnvme_dev_export(device->dev, &res->ipc)
  recv homi_res_dev_connect    ◀───  send blob (no fds)
  xnvme_dev_import(&res->ipc)
  /* shell_dev: geo/ident only */

homic_queue_connect(uri, cap, shell_dev)
  send QUEUE_CONNECT {uri,cap} ───▶  xnvme_queue_init_shared(...)
                                      allocates SQ/CQ in queue_heap_fd (memfd)
                                      issues Create CQ / Create SQ admin cmds
                                      allocates client_heap_fd
  recv response + fds          ◀───  send res + SCM_RIGHTS{queue_heap_fd,
    queue_heap_fd, bar_fd,                                  bar_fd,
    client_heap_fd                                          client_heap_fd}
  xnvme_queue_from_ipc(...)
  /* queue: SQ/CQ in shared hugepages, doorbells via BAR mmap */

/* I/O — no daemon involvement */
buf = xnvme_buf_alloc(shell_dev, nbytes)
ctx = xnvme_queue_get_cmd_ctx(queue)
xnvme_nvm_read(shell_dev, queue, ctx, slba, nlb, buf, NULL, 0)
xnvme_queue_poke(queue, 0)
```

## Implementation order

1. `xnvme_dev_export` / `xnvme_dev_import` in xNVMe.
2. `hostmem_hugepage_alloc_fd` and `hostmem_heap_import` in upcie hostmem.
3. `struct xnvme_queue_ipc` / `struct xnvme_heap_ipc` in `libxnvme_ipc.h`.
4. `xnvme_queue_init_shared` and `xnvme_queue_from_ipc` in xNVMe.
5. `homi_proto` fd passing (switch to sendmsg/recvmsg).
6. `DEV_CONNECT` and `QUEUE_CONNECT` in `homi_proto.h`.
7. Both new cases in `homid_ipc.c`.
8. `homic_dev_connect` and `homic_queue_connect` in `homic.c` / `homic.h`.
9. Update `homi/docs/ipc.md` and `homi/docs/client.md`.

## Out of scope

- **Queue lifetime management.** Provisioned queues live until the daemon exits.
  A `QUEUE_DISCONNECT` message allowing early cleanup is future work.
- **ublk integration** (kernel consumer path) is a separate effort; nothing here
  conflicts with it.
- **SR-IOV path.** Each initiator opens its own VF directly; `QUEUE_CONNECT` is
  not needed. `DEV_CONNECT` is still useful for metadata without Identify commands.
- **Multi-queue per client.** Nothing prevents calling `homic_queue_connect`
  multiple times, but the daemon has no per-client session concept.
