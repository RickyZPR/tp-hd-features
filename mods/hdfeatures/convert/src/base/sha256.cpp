// SHA-256 (C2): Windows CNG (BCryptHash, system bcrypt.dll) in the library and the mod; a plain FIPS 180-4
// implementation elsewhere, and as the fallback if CNG ever refuses (sha256_portable.hpp, cross-checked
// against CNG by self-test). Never aborts: the mod must not take the game down over a hash.
#include "hdf/source.hpp"

#include "sha256_portable.hpp"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace hdf {

#ifdef _WIN32
namespace {
BCRYPT_ALG_HANDLE sha256_algorithm() {
#ifdef BCRYPT_SHA256_ALG_HANDLE
    return BCRYPT_SHA256_ALG_HANDLE;  // pseudo-handle (Windows 10+): nothing to open or close, thread-safe
#else
    static BCRYPT_ALG_HANDLE alg = [] {
        BCRYPT_ALG_HANDLE h = nullptr;
        if (BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) h = nullptr;
        return h;
    }();
    return alg;
#endif
}
}  // namespace
#endif

struct Sha256::Impl {
#ifdef _WIN32
    BCRYPT_HASH_HANDLE hash = nullptr;  // nullptr: CNG refused, the portable code is used
    ~Impl() {
        if (hash) BCryptDestroyHash(hash);
    }
#endif
    base::Sha256Portable portable;
    bool finished = false;
};

Sha256::Sha256() : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    // CNG allocates the hash object itself (pbHashObject = nullptr, Windows 7+).
    if (BCryptCreateHash(sha256_algorithm(), &impl_->hash, nullptr, 0, nullptr, 0, 0) < 0) impl_->hash = nullptr;
#endif
}

Sha256::~Sha256() = default;

void Sha256::update(View data) {
    if (impl_->finished || data.empty()) return;
#ifdef _WIN32
    if (impl_->hash) {
        const u8* p = data.data();
        std::size_t n = data.size();
        while (n > 0) {
            const ULONG take = static_cast<ULONG>(n < (std::size_t(1) << 30) ? n : (std::size_t(1) << 30));
            if (BCryptHashData(impl_->hash, const_cast<PUCHAR>(p), take, 0) < 0) {
                // Not expected. Keep going with a result that can never match a real hash.
                BCryptDestroyHash(impl_->hash);
                impl_->hash = nullptr;
                impl_->finished = true;
                return;
            }
            p += take;
            n -= take;
        }
        return;
    }
#endif
    impl_->portable.update(data.data(), data.size());
}

std::array<u8, 32> Sha256::finish() {
    std::array<u8, 32> out{};
    if (impl_->finished) return out;  // all zero: never equal to a listed hash
    impl_->finished = true;
#ifdef _WIN32
    if (impl_->hash) {
        if (BCryptFinishHash(impl_->hash, out.data(), static_cast<ULONG>(out.size()), 0) < 0) out = {};
        return out;
    }
#endif
    return impl_->portable.finish();
}

std::string Sha256::finish_hex() {
    const std::array<u8, 32> d = finish();
    return to_hex(d);
}

std::string sha256_hex(View data) {
    Sha256 h;
    h.update(data);
    return h.finish_hex();
}

}  // namespace hdf
