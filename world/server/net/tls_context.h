// Server TLS context. Uses TLS_CERT_FILE/TLS_KEY_FILE when set; otherwise generates a self-signed dev certificate
// into TLS_DEV_CERT_DIR on first start and reuses it afterwards (mirrors the dev-key fallback in auth).
#pragma once
#include <asio/ssl.hpp>

#include <string>

namespace chthonic::net {

struct TlsFiles {
    std::string certFile, keyFile; // both empty = dev certificate
    std::string devCertDir;
};

// Throws std::runtime_error when the files can't be read or the certificate can't be generated. A dev certificate made
// before it named the chat service is made again.
asio::ssl::context make_server_tls(const TlsFiles& files);
// A client context that trusts caFile (PEM; empty: the system's CAs) and checks the peer's certificate names host.
// Throws std::runtime_error if caFile can't be read.
asio::ssl::context make_client_tls(const std::string& caFile, const std::string& host);

} // namespace chthonic::net
