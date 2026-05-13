#ifndef HOMIC_H
#define HOMIC_H

#include <libxal.h>
#include <libxnvme.h>
#include <libxnvme_ipc.h>

/**
 * Connect to the homid daemon.
 *
 * Opens a Unix domain socket connection to the daemon. Must be called before
 * any other homic functions. The connection is held globally; call
 * homic_disconnect() to release it.
 *
 * @return  0 on success, negative errno on failure.
 */
int
homic_connect(char *socket_path);

/**
 * Disconnect from the homid daemon.
 *
 * Closes the socket and releases the global connection. Safe to call if not
 * connected.
 */
void
homic_disconnect();

/**
 * Connect to xal for a specific device.
 *
 * Sends an XAL_CONNECT request to the daemon, maps the inode and extent pools
 * from POSIX shared memory, and constructs a read-only xal via xal_from_pools().
 * Requires an active connection established with homic_connect().
 *
 * @param dev_uri  URI of the device to connect to.
 * @param out      Output: read-only xal struct backed by shared memory.
 * @return         0 on success, negative errno on failure.
 */
int
homic_connect_xal(char *dev_uri, struct xal **out);

/**
 * Wait until the xal pools are not being reindexed.
 *
 * Spins while the daemon is running xal_index(). Returns once it is safe to
 * read from the xal pools. Requires an active connection established with
 * homic_connect().
 *
 * @param xal  xal instance to wait on.
 * @return     0 on success, negative errno on failure.
 */
int
homic_xal_wait(struct xal *xal);

/**
 * Request device metadata from the daemon.
 *
 * Sends a DEV_CONNECT request for dev_uri and constructs a shell xnvme_dev
 * from the returned metadata blob. The shell is sufficient for command
 * construction but cannot open queues or issue admin commands directly.
 * Close it with xnvme_dev_close() when done.
 *
 * @param dev_uri  URI of the device to connect to.
 * @param out      Output: shell xnvme_dev backed by daemon metadata.
 * @return         0 on success, negative errno on failure.
 */
int
homic_dev_connect(char *dev_uri, struct xnvme_dev **out);

/**
 * Request a pre-provisioned NVMe queue pair from the daemon.
 *
 * Sends a QUEUE_CONNECT request, receives the queue descriptor and three file
 * descriptors via SCM_RIGHTS, and calls xnvme_queue_from_ipc() to map the
 * shared SQ/CQ memory and wire up the uPCIe async backend on shell_dev.
 * After this call, I/O can be submitted through the returned queue without
 * daemon involvement. Close the queue with xnvme_queue_term() when done.
 *
 * @param dev_uri   URI of the device to connect to.
 * @param capacity  Requested queue depth.
 * @param shell_dev Shell device created by homic_dev_connect().
 * @param out       Output: reconstructed xnvme_queue ready for async I/O.
 * @return          0 on success, negative errno on failure.
 */
int
homic_queue_connect(char *dev_uri, uint16_t capacity, struct xnvme_dev *shell_dev,
		    struct xnvme_queue **out);

#endif /* HOMIC_H */
