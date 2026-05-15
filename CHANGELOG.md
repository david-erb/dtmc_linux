# Changelog

## v1.3.0 - 2026-05-15

- **API change**: `post_callback` and `post_callback_context` removed from `dthttpd_linux_socket_config_t`; set the POST callback at runtime with `dthttpd_linux_socket_set_callback()` instead.
- New `dthttpclient` Linux backend (`dthttpclient_linux.c`, backed by libcurl) implementing `dthttpclient_create`, `dthttpclient_get`, `dthttpclient_post`, and `dthttpclient_dispose`; libcurl is now a required dependency.
- New `dthttpd_linux_socket_set_callback()` for attaching a POST callback after construction.
- New `dthttpd_linux_socket_concat_format()` for building a formatted URL/webroot summary string.
- Fix: websocket server keeps `listen_fd` alive across client reconnects instead of rebinding on each connection.
- Fix: websocket write detects graceful peer close (TCP FIN or WS close frame) via `MSG_PEEK` before sending, returning `DTERR_IO` cleanly instead of a broken pipe.
