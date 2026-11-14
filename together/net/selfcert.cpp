// See selfcert.h.
#include "selfcert.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <sys/stat.h>
#include <unistd.h>

namespace selfcert {
namespace {

// A new file only its owner can read, replacing any old one.
FILE *OpenPrivate(const std::string &path) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return nullptr;
    if (fchmod(fd, 0600) != 0) {
        close(fd);
        return nullptr;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) close(fd);
    return f;
}

}  // namespace

bool Make(const std::string &keyPath, const std::string &certPath, std::string &err) {
    EVP_PKEY *key = EVP_EC_gen("P-256");
    X509 *cert = X509_new();
    bool ok = key && cert;
    if (ok) {
        unsigned char serial[16];
        ok = RAND_bytes(serial, sizeof serial) == 1;
        serial[0] &= 0x7f;   // a positive number
        BIGNUM *bn = ok ? BN_bin2bn(serial, sizeof serial, nullptr) : nullptr;
        ok = bn && BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert));
        BN_free(bn);
    }
    if (ok) {
        X509_set_version(cert, 2);
        X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
        X509_gmtime_adj(X509_getm_notAfter(cert), 10L * 365 * 24 * 3600);
        X509_NAME *name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("frametop-together"),
                                   -1, -1, 0);
        ok = X509_set_issuer_name(cert, name) == 1 && X509_set_pubkey(cert, key) == 1 &&
             X509_sign(cert, key, EVP_sha256()) > 0;
    }
    if (!ok) err = "could not make a certificate";
    if (ok) {
        FILE *f = OpenPrivate(keyPath);
        ok = f && PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
        if (f && std::fclose(f) != 0) ok = false;
        if (!ok) err = keyPath + ": " + strerror(errno);
    }
    if (ok) {
        FILE *f = std::fopen(certPath.c_str(), "w");
        ok = f && PEM_write_X509(f, cert) == 1;
        if (f && std::fclose(f) != 0) ok = false;
        if (!ok) err = certPath + ": " + strerror(errno);
    }
    X509_free(cert);
    EVP_PKEY_free(key);
    return ok;
}

}  // namespace selfcert
