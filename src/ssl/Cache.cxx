/*
 * SSL/TLS certificate database and cache.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "Cache.hxx"
#include "SessionCache.hxx"
#include "Basic.hxx"
#include "ssl/Name.hxx"
#include "ssl/Error.hxx"
#include "ssl/LoadFile.hxx"
#include "certdb/Wildcard.hxx"
#include "pg/CheckError.hxx"
#include "util/AllocatedString.hxx"

#include <openssl/err.h>

unsigned
CertCache::FlushSessionCache(long tm)
{
    unsigned n = 0;

    for (auto &i : map)
        n += ::FlushSessionCache(*i.second.ssl_ctx, tm);

    return n;
}

void
CertCache::Expire()
{
    const auto now = std::chrono::steady_clock::now();

    for (auto i = map.begin(), end = map.end(); i != end;) {
        if (now >= i->second.expires) {
            logger(5, "flushed certificate '", i->first, "'");
            i = map.erase(i);
        } else
            ++i;
    }
}

void
CertCache::LoadCaCertificate(const char *path)
{
    auto chain = LoadCertChainFile(path);
    assert(!chain.empty());

    X509_NAME *subject = X509_get_subject_name(chain.front().get());
    if (subject == nullptr)
        throw SslError(std::string("CA certificate has no subject: ") + path);

    auto digest = CalcSHA1(*subject);
    auto r = ca_certs.emplace(std::move(digest), std::move(chain));
    if (!r.second)
        throw SslError(std::string("Duplicate CA certificate: ") + path);
}

SslCtx
CertCache::Add(UniqueX509 &&cert, UniqueEVP_PKEY &&key)
{
    assert(cert);
    assert(key);

    auto ssl_ctx = CreateBasicSslCtx(true);
    // TODO: call ApplyServerConfig()

    ERR_clear_error();

    const auto name = GetCommonName(*cert);

    X509_NAME *issuer = X509_get_issuer_name(cert.get());

    if (SSL_CTX_use_PrivateKey(ssl_ctx.get(), key.get()) != 1)
        throw SslError("SSL_CTX_use_PrivateKey() failed");

    if (SSL_CTX_use_certificate(ssl_ctx.get(), cert.get()) != 1)
        throw SslError("SSL_CTX_use_certificate() failed");

    if (issuer != nullptr) {
        auto i = ca_certs.find(CalcSHA1(*issuer));
        if (i != ca_certs.end())
            for (const auto &ca_cert : i->second)
                SSL_CTX_add_extra_chain_cert(ssl_ctx.get(),
                                             X509_dup(ca_cert.get()));
    }

    if (name != nullptr) {
        const std::unique_lock<std::mutex> lock(mutex);
        map.emplace(name.c_str(), ssl_ctx);
    }

    return ssl_ctx;
}

SslCtx
CertCache::Query(const char *host)
{
    auto db = dbs.Get(config);
    db->EnsureConnected();

    auto cert_key = db->GetServerCertificateKey(host);
    if (!cert_key.second)
        return SslCtx();

    return Add(std::move(cert_key.first), std::move(cert_key.second));
}

SslCtx
CertCache::GetNoWildCard(const char *host)
{
    {
        const std::unique_lock<std::mutex> lock(mutex);
        auto i = map.find(host);
        if (i != map.end()) {
            i->second.expires = std::chrono::steady_clock::now() + std::chrono::hours(24);
            return i->second.ssl_ctx;
        }
    }

    if (name_cache.Lookup(host)) {
        auto ssl_ctx = Query(host);
        if (ssl_ctx)
            return ssl_ctx;
    }

    return {};
}

SslCtx
CertCache::Get(const char *host)
{
    auto ssl_ctx = GetNoWildCard(host);
    if (!ssl_ctx) {
        /* not found: try the wildcard */
        const auto wildcard = MakeCommonNameWildcard(host);
        if (!wildcard.empty())
            ssl_ctx = GetNoWildCard(wildcard.c_str());
    }

    return ssl_ctx;
}

void
CertCache::OnCertModified(const std::string &name, bool deleted)
{
    const std::unique_lock<std::mutex> lock(mutex);
    auto i = map.find(name);
    if (i != map.end()) {
        map.erase(i);

        logger.Format(5, "flushed %s certificate '%s'",
                      deleted ? "deleted" : "modified",
                      name.c_str());
    }
}
