# TODO

- [ ] Add support for HTTP keep-alive connections
  - Currently, `handle_connection` processes only one request per connection and closes the socket
  - Should loop to handle multiple requests on the same connection when keep-alive is enabled
  - Need to handle connection timeout/idle timeout for keep-alive connections

- [ ] Add support for TLS
  - Currently, TLS is not supported.
  - Probably use OpenSSL as an optional config.
  - Need to be able to get cert information from the connection for auth
