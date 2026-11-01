/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HTTP management API over libmicrohttpd (websrv model). PS5 build only. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Start the management API on port. Blocks; call in its own thread.
   backend must outlive the daemon. Returns 0 on success. */
int http_api_start(int port, void *backend);

/* Stop the daemon. */
void http_api_stop(void);

#ifdef __cplusplus
}
#endif
