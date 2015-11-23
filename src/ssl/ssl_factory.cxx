/*
 * SSL/TLS configuration.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "ssl_factory.hxx"
#include "ssl_config.hxx"
#include "ssl_domain.hxx"
#include "Util.hxx"
#include "util/Error.hxx"

#include <inline/compiler.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <algorithm>

#include <assert.h>

struct ssl_cert_key {
    SSL_CTX *ssl_ctx = nullptr;

    char *common_name = nullptr;
    size_t cn_length;

    ssl_cert_key() = default;

    ssl_cert_key(ssl_cert_key &&other)
        :ssl_ctx(other.ssl_ctx),
         common_name(other.common_name),
         cn_length(other.cn_length) {
        other.ssl_ctx = nullptr;
        other.common_name = nullptr;
    }

    ~ssl_cert_key() {
        if (ssl_ctx != nullptr)
            SSL_CTX_free(ssl_ctx);
        delete[] common_name;
    }

    ssl_cert_key &operator=(ssl_cert_key &&other) {
        std::swap(ssl_ctx, other.ssl_ctx);
        std::swap(common_name, other.common_name);
        cn_length = other.cn_length;
        return *this;
    }

    bool LoadClient(Error &error);

    bool LoadServer(const ssl_config &parent_config,
                    const ssl_cert_key_config &config, Error &error);

    void CacheCommonName(X509_NAME *subject) {
        char buffer[256];
        int len = X509_NAME_get_text_by_NID(subject, NID_commonName, buffer,
                                            sizeof(buffer));
        if (len < 0)
            return;

        cn_length = len;
        common_name = new char[cn_length + 1];
        std::copy_n(buffer, cn_length + 1, common_name);
    }

    void CacheCommonName(X509 *cert) {
        assert(common_name == nullptr);

        X509_NAME *subject = X509_get_subject_name(cert);
        if (subject != nullptr)
            CacheCommonName(subject);
    }

    gcc_pure
    bool MatchCommonName(const char *host_name, size_t hn_length) const;

    void Apply(SSL *ssl) const {
        SSL_set_SSL_CTX(ssl, ssl_ctx);
    }

    unsigned Flush(long tm);
};

struct ssl_factory {
    std::vector<ssl_cert_key> cert_key;

    const bool server;

    explicit ssl_factory(bool _server)
        :server(_server) {}

    bool EnableSNI(Error &error);

    SSL *Make();

    unsigned Flush(long tm);
};

static int
verify_callback(int ok, gcc_unused X509_STORE_CTX *ctx)
{
    return ok;
}

static bool
load_certs_keys(ssl_factory &factory, const ssl_config &config,
                Error &error)
{
    factory.cert_key.reserve(config.cert_key.size());

    for (const auto &c : config.cert_key) {
        ssl_cert_key ck;
        if (!ck.LoadServer(config, c, error))
            return false;

        factory.cert_key.emplace_back(std::move(ck));
    }

    return true;
}

static bool
apply_server_config(SSL_CTX *ssl_ctx, const ssl_config &config,
                    const ssl_cert_key_config &cert_key,
                    Error &error)
{
    ERR_clear_error();

    if (SSL_CTX_use_RSAPrivateKey_file(ssl_ctx,
                                       cert_key.key_file.c_str(),
                                       SSL_FILETYPE_PEM) != 1) {
        ERR_print_errors_fp(stderr);
        error.Format(ssl_domain, "Failed to load key file %s",
                     cert_key.key_file.c_str());
        return false;
    }

    if (SSL_CTX_use_certificate_chain_file(ssl_ctx,
                                           cert_key.cert_file.c_str()) != 1) {
        ERR_print_errors_fp(stderr);
        error.Format(ssl_domain, "Failed to load certificate file %s",
                     cert_key.cert_file.c_str());
        return false;
    }

    if (!config.ca_cert_file.empty()) {
        if (SSL_CTX_load_verify_locations(ssl_ctx,
                                          config.ca_cert_file.c_str(),
                                          nullptr) != 1) {
            error.Format(ssl_domain, "Failed to load CA certificate file %s",
                         config.ca_cert_file.c_str());
            return false;
        }

        /* send all certificates from this file to the client (list of
           acceptable CA certificates) */

        STACK_OF(X509_NAME) *list =
            SSL_load_client_CA_file(config.ca_cert_file.c_str());
        if (list == nullptr) {
            error.Format(ssl_domain,
                         "Failed to load CA certificate list from file %s",
                         config.ca_cert_file.c_str());
            return false;
        }

        SSL_CTX_set_client_CA_list(ssl_ctx, list);
    }

    if (config.verify != ssl_verify::NO) {
        /* enable client certificates */
        int mode = SSL_VERIFY_PEER;

        if (config.verify == ssl_verify::YES)
            mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;

        SSL_CTX_set_verify(ssl_ctx, mode, verify_callback);
    }

    return true;
}

inline bool
ssl_cert_key::MatchCommonName(const char *host_name, size_t hn_length) const
{
    if (common_name == nullptr)
        return false;

    if (strcmp(host_name, common_name) == 0)
        return true;

    if (common_name[0] == '*' && common_name[1] == '.' &&
        common_name[2] != 0) {
        if (hn_length >= cn_length &&
            /* match only one segment (no dots) */
            memchr(host_name, '.', hn_length - cn_length + 1) == nullptr &&
            memcmp(host_name + hn_length - cn_length + 1,
                   common_name + 1, cn_length - 1) == 0)
            return true;
    }

    return false;
}

static int
ssl_servername_callback(SSL *ssl, gcc_unused int *al,
                        const ssl_factory &factory)
{
    const char *host_name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (host_name == nullptr)
        return SSL_TLSEXT_ERR_OK;

    const size_t length = strlen(host_name);

    /* find the first certificate that matches */

    for (const auto &ck : factory.cert_key) {
        if (ck.MatchCommonName(host_name, length)) {
            /* found it - now use it */
            ck.Apply(ssl);
            break;
        }
    }

    return SSL_TLSEXT_ERR_OK;
}

inline bool
ssl_factory::EnableSNI(Error &error)
{
    SSL_CTX *ssl_ctx = cert_key.front().ssl_ctx;

    if (!SSL_CTX_set_tlsext_servername_callback(ssl_ctx,
                                                ssl_servername_callback) ||
        !SSL_CTX_set_tlsext_servername_arg(ssl_ctx, this)) {
        error.Format(ssl_domain,
                     "SSL_CTX_set_tlsext_servername_callback() failed");
        return false;
    }

    return true;
}

inline SSL *
ssl_factory::Make()
{
    SSL *ssl = SSL_new(cert_key.front().ssl_ctx);
    if (ssl == nullptr)
        return nullptr;

    if (server)
        SSL_set_accept_state(ssl);
    else
        SSL_set_connect_state(ssl);

    return ssl;
}

inline unsigned
ssl_cert_key::Flush(long tm)
{
    unsigned before = SSL_CTX_sess_number(ssl_ctx);
    SSL_CTX_flush_sessions(ssl_ctx, tm);
    unsigned after = SSL_CTX_sess_number(ssl_ctx);
    return after < before ? before - after : 0;
}

inline unsigned
ssl_factory::Flush(long tm)
{
    unsigned n = 0;
    for (auto &i : cert_key)
        n += i.Flush(tm);
    return n;
}

/**
 * Enable Elliptic curve Diffie-Hellman (ECDH) for perfect forward
 * secrecy.  By default, it OpenSSL disables it.
 */
static bool
enable_ecdh(SSL_CTX *ssl_ctx, Error &error)
{
    /* OpenSSL 1.0.2 will allow this instead:

       SSL_CTX_set_ecdh_auto(ssl_ctx, 1)
    */

    EC_KEY *ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (ecdh == nullptr) {
        error.Set(ssl_domain, "EC_KEY_new_by_curve_name() failed");
        return false;
    }

    bool success = SSL_CTX_set_tmp_ecdh(ssl_ctx, ecdh) == 1;
    EC_KEY_free(ecdh);
    if (!success)
        error.Set(ssl_domain, "SSL_CTX_set_tmp_ecdh() failed");

    return success;
}

static bool
SetupBasicSslCtx(SSL_CTX *ssl_ctx, bool server, Error &error)
{
    long mode = SSL_MODE_ENABLE_PARTIAL_WRITE
        | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER;
#ifdef SSL_MODE_RELEASE_BUFFERS
    /* requires libssl 1.0.0 */
    mode |= SSL_MODE_RELEASE_BUFFERS;
#endif

    /* without this flag, OpenSSL attempts to verify the whole local
       certificate chain for each connection, which is a waste of CPU
       time */
    mode |= SSL_MODE_NO_AUTO_CHAIN;

    SSL_CTX_set_mode(ssl_ctx, mode);

    if (server && !enable_ecdh(ssl_ctx, error))
        return false;

    /* disable protocols that are known to be insecure */
    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);

    /* disable weak ciphers */
    SSL_CTX_set_cipher_list(ssl_ctx, "DEFAULT:!EXPORT:!LOW");

    return true;
}

static SSL_CTX *
CreateBasicSslCtx(bool server, Error &error)
{
    ERR_clear_error();

    /* don't be fooled - we want TLS, not SSL - but TLSv1_method()
       will only allow TLSv1.0 and will refuse TLSv1.1 and TLSv1.2;
       only SSLv23_method() supports all (future) TLS protocol
       versions, even if we don't want any SSL at all */
    auto method = server
        ? SSLv23_server_method()
        : SSLv23_client_method();

    SSL_CTX *ssl_ctx = SSL_CTX_new(method);
    if (ssl_ctx == nullptr) {
        ERR_print_errors_fp(stderr);
        error.Format(ssl_domain, "SSL_CTX_new() failed");
        return nullptr;
    }

    if (!SetupBasicSslCtx(ssl_ctx, server, error)) {
        SSL_CTX_free(ssl_ctx);
        return nullptr;
    }

    return ssl_ctx;
}

bool
ssl_cert_key::LoadClient(Error &error)
{
    assert(ssl_ctx == nullptr);

    ssl_ctx = CreateBasicSslCtx(false, error);
    return ssl_ctx != nullptr;
}

bool
ssl_cert_key::LoadServer(const ssl_config &parent_config,
                         const ssl_cert_key_config &config, Error &error)
{
    assert(ssl_ctx == nullptr);

    ssl_ctx = CreateBasicSslCtx(true, error);
    if (ssl_ctx == nullptr)
        return false;

    assert(!parent_config.cert_key.empty());

    if (!apply_server_config(ssl_ctx, parent_config, config,
                             error))
        return false;

    SSL *ssl = SSL_new(ssl_ctx);
    if (ssl == nullptr) {
        error.Format(ssl_domain, "SSL_new() failed");
        return false;
    }

    X509 *cert = SSL_get_certificate(ssl);
    EVP_PKEY *key = SSL_get_privatekey(ssl);
    if (cert == nullptr || key == nullptr) {
        SSL_free(ssl);
        error.Set(ssl_domain, "No cert/key in SSL_CTX");
        return false;
    }

    if (!MatchModulus(*cert, *key)) {
        SSL_free(ssl);
        error.Format(ssl_domain, "Key '%s' does not match certificate '%s'",
                     config.key_file.c_str(), config.cert_file.c_str());
        return false;
    }

    CacheCommonName(cert);
    SSL_free(ssl);

    return true;
}

struct ssl_factory *
ssl_factory_new(const ssl_config &config,
                bool server,
                Error &error)
{
    assert(!config.cert_key.empty() || !server);

    auto *factory = new ssl_factory(server);

    if (server) {
        assert(!config.cert_key.empty());

        if (!load_certs_keys(*factory, config, error)) {
            delete factory;
            return nullptr;
        }
    } else {
        assert(config.cert_key.empty());
        assert(config.ca_cert_file.empty());
        assert(config.verify == ssl_verify::NO);

        factory->cert_key.emplace_back();
        if (!factory->cert_key.front().LoadClient(error)) {
            delete factory;
            return nullptr;
        }
    }

    if (factory->cert_key.size() > 1 && !factory->EnableSNI(error)) {
        delete factory;
        return nullptr;
    }

    return factory;
}

void
ssl_factory_free(struct ssl_factory *factory)
{
    delete factory;
}

SSL *
ssl_factory_make(ssl_factory &factory)
{
    return factory.Make();
}

unsigned
ssl_factory_flush(struct ssl_factory &factory, long tm)
{
    return factory.Flush(tm);
}
