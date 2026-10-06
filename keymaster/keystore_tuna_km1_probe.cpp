/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <errno.h>
#include <AndroidKeymaster3Device.h>
#include <hardware/keymaster1.h>
#include <dlfcn.h>
#include <stdio.h>
#include <time.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <algorithm>
#include <initializer_list>
#include <memory>
#include <vector>

namespace {
namespace hal = android::hardware::keymaster::V3_0;
using android::hardware::hidl_vec;
using Bytes = hidl_vec<uint8_t>;
using Params = hidl_vec<hal::KeyParameter>;
using UniquePkey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using UniqueContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
static uint64_t milliseconds() {
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}
class Results {
public:
    template<class Action> void check(const char* name, Action action) {
        uint64_t started = milliseconds();
        bool success = action();
        printf("%s %s %llu ms\n", success ? "PASS" : "FAIL", name,
               static_cast<unsigned long long>(milliseconds() - started));
        failed |= !success;
    }
    bool failed = false;
};
static hal::KeyParameter scalar(keymaster_tag_t tag, uint64_t value) {
    hal::KeyParameter param = {};
    param.tag = static_cast<hal::Tag>(tag);
    switch (tag) {
    case KM_TAG_ALGORITHM: param.f.algorithm = static_cast<hal::Algorithm>(value); break;
    case KM_TAG_PURPOSE: param.f.purpose = static_cast<hal::KeyPurpose>(value); break;
    case KM_TAG_DIGEST: param.f.digest = static_cast<hal::Digest>(value); break;
    case KM_TAG_PADDING: param.f.paddingMode = static_cast<hal::PaddingMode>(value); break;
    case KM_TAG_BLOCK_MODE: param.f.blockMode = static_cast<hal::BlockMode>(value); break;
    case KM_TAG_EC_CURVE: param.f.ecCurve = static_cast<hal::EcCurve>(value); break;
    case KM_TAG_RSA_PUBLIC_EXPONENT: param.f.longInteger = value; break;
    case KM_TAG_NO_AUTH_REQUIRED: case KM_TAG_CALLER_NONCE: param.f.boolValue = true; break;
    default: param.f.integer = value; break;
    }
    return param;
}
static Params parameters(std::initializer_list<hal::KeyParameter> values) {
    Params result; result.resize(values.size());
    std::copy(values.begin(), values.end(), result.begin()); return result;
}
static Bytes bytes(const uint8_t* data, size_t length) {
    Bytes result; result.resize(length);
    if (length) std::copy(data, data + length, result.begin());
    return result;
}
static Bytes message() {
    static const uint8_t text[] = "tuna keymaster HAL lifecycle probe";
    return bytes(text, sizeof(text) - 1);
}
static void append(Bytes* destination, const Bytes& source) {
    size_t previous = destination->size(); destination->resize(previous + source.size());
    std::copy(source.begin(), source.end(), destination->begin() + previous);
}
static bool equal(const Bytes& left, const Bytes& right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}
class Session {
public:
    explicit Session(hw_module_t* module) : module_(module) {}
    ~Session() { close(); }
    bool open() {
        hw_device_t* hardware = nullptr;
        if (module_->methods->open(module_, KEYSTORE_KEYMASTER, &hardware) != 0 || !hardware) return false;
        auto* device = reinterpret_cast<keymaster1_device_t*>(hardware);
        // CreateKeymasterDevice transfers device ownership to Keymaster1Engine.
        hal_ = ::keymaster::ng::CreateKeymasterDevice(device);
        if (!hal_) { hardware->close(hardware); return false; }
        return true;
    }
    void close() { hal_.clear(); }
    hal::IKeymasterDevice* get() const { return hal_.get(); }
private:
    hw_module_t* module_;
    android::sp<hal::IKeymasterDevice> hal_;
};
class Keys {
public:
    explicit Keys(Session* session) : session_(session) {}
    ~Keys() {
        if (!session_->get() && !session_->open()) {
            if (!blobs.empty()) fprintf(stderr, "FAIL cleanup-reopen 0 ms\n");
            return;
        }
        for (const auto& blob : blobs) {
            auto status = session_->get()->deleteKey(blob);
            if (!status.isOk() || static_cast<hal::ErrorCode>(status) != hal::ErrorCode::OK)
                fprintf(stderr, "FAIL cleanup-delete 0 ms\n");
        }
    }
    void retain(const Bytes& blob) { if (blob.size()) blobs.push_back(blob); }
    Session* session_;
    std::vector<Bytes> blobs;
};
static bool create(Session& session, Keys& keys, const Params& params, Bytes* blob,
                   const Bytes* imported = nullptr) {
    if (!session.get()) return false;
    bool success = false;
    auto callback = [&](hal::ErrorCode error, const Bytes& created, const hal::KeyCharacteristics&) {
        keys.retain(created);
        success = error == hal::ErrorCode::OK && created.size() > 0;
        if (success) *blob = created;
    };
    if (imported) return session.get()->importKey(params, hal::KeyFormat::PKCS8, *imported, callback).isOk() && success;
    return session.get()->generateKey(params, callback).isOk() && success;
}
static UniquePkey public_key(Session& session, const Bytes& blob) {
    Bytes encoded;
    bool success = false;
    if (!session.get() || !session.get()->exportKey(hal::KeyFormat::X509, blob, {}, {},
        [&](hal::ErrorCode error, const Bytes& output) { success = error == hal::ErrorCode::OK; encoded = output; }).isOk() || !success)
        return UniquePkey(nullptr, EVP_PKEY_free);
    const uint8_t* cursor = encoded.data();
    UniquePkey key(d2i_PUBKEY(nullptr, &cursor, encoded.size()), EVP_PKEY_free);
    if (cursor != encoded.data() + encoded.size()) key.reset();
    return key;
}
static bool operate(Session& session, hal::KeyPurpose purpose, const Bytes& blob, const Params& params,
                    const Bytes& input, const Bytes& signature, Bytes* output, Params* begin_output = nullptr) {
    if (!session.get() || blob.size() == 0) return false;
    uint64_t handle = 0;
    bool success = false;
    auto started = session.get()->begin(purpose, blob, params,
        [&](hal::ErrorCode error, const Params& returned, uint64_t created) {
            success = error == hal::ErrorCode::OK; handle = created;
            if (begin_output) *begin_output = returned;
        });
    if (!started.isOk() || !success) {
        if (handle) (void)session.get()->abort(handle);
        return false;
    }
    *output = {};
    size_t offset = 0;
    while (offset < input.size()) {
        uint32_t consumed = 0;
        Bytes remaining = bytes(input.data() + offset, input.size() - offset);
        success = false;
        auto updated = session.get()->update(handle, {}, remaining,
            [&](hal::ErrorCode error, uint32_t used, const Params&, const Bytes& produced) {
                success = error == hal::ErrorCode::OK; consumed = used; append(output, produced);
            });
        if (!updated.isOk() || !success || consumed == 0 || consumed > remaining.size()) {
            (void)session.get()->abort(handle); return false;
        }
        offset += consumed;
    }
    success = false;
    auto finished = session.get()->finish(handle, {}, {}, signature,
        [&](hal::ErrorCode error, const Params&, const Bytes& produced) {
            success = error == hal::ErrorCode::OK; append(output, produced);
        });
    if (!finished.isOk() || !success) { (void)session.get()->abort(handle); return false; }
    return true;
}
static Params rsa_params() {
    return parameters({scalar(KM_TAG_ALGORITHM, KM_ALGORITHM_RSA), scalar(KM_TAG_KEY_SIZE, 2048),
        scalar(KM_TAG_RSA_PUBLIC_EXPONENT, 65537), scalar(KM_TAG_PURPOSE, KM_PURPOSE_SIGN),
        scalar(KM_TAG_PURPOSE, KM_PURPOSE_VERIFY), scalar(KM_TAG_PURPOSE, KM_PURPOSE_ENCRYPT),
        scalar(KM_TAG_PURPOSE, KM_PURPOSE_DECRYPT), scalar(KM_TAG_DIGEST, KM_DIGEST_NONE),
        scalar(KM_TAG_DIGEST, KM_DIGEST_SHA_2_256), scalar(KM_TAG_PADDING, KM_PAD_NONE),
        scalar(KM_TAG_PADDING, KM_PAD_RSA_PKCS1_1_5_SIGN), scalar(KM_TAG_PADDING, KM_PAD_RSA_PSS),
        scalar(KM_TAG_PADDING, KM_PAD_RSA_PKCS1_1_5_ENCRYPT), scalar(KM_TAG_PADDING, KM_PAD_RSA_OAEP),
        scalar(KM_TAG_NO_AUTH_REQUIRED, 1)});
}
static Params rsa_operation(keymaster_padding_t padding) {
    return parameters({scalar(KM_TAG_DIGEST, padding == KM_PAD_RSA_PKCS1_1_5_ENCRYPT ? KM_DIGEST_NONE : KM_DIGEST_SHA_2_256),
                       scalar(KM_TAG_PADDING, padding)});
}
static bool sign_verify(Session& session, const Bytes& blob, EVP_PKEY* public_key, keymaster_padding_t padding,
                        bool elliptic = false) {
    if (!public_key) return false;
    Bytes signature;
    Params params = elliptic ? parameters({scalar(KM_TAG_DIGEST, KM_DIGEST_SHA_2_256)}) : rsa_operation(padding);
    Bytes input = message();
    if (!operate(session, hal::KeyPurpose::SIGN, blob, params, input, {}, &signature)) return false;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_PKEY_CTX* key_context = nullptr;
    if (!context || EVP_DigestVerifyInit(context.get(), &key_context, EVP_sha256(), nullptr, public_key) != 1) return false;
    if (!elliptic && EVP_PKEY_CTX_set_rsa_padding(key_context,
        padding == KM_PAD_RSA_PSS ? RSA_PKCS1_PSS_PADDING : RSA_PKCS1_PADDING) != 1) return false;
    if (padding == KM_PAD_RSA_PSS &&
        (EVP_PKEY_CTX_set_rsa_pss_saltlen(key_context, 32) != 1 || EVP_PKEY_CTX_set_rsa_mgf1_md(key_context, EVP_sha256()) != 1)) return false;
    return EVP_DigestVerifyUpdate(context.get(), input.data(), input.size()) == 1 &&
           EVP_DigestVerifyFinal(context.get(), signature.data(), signature.size()) == 1;
}
static bool rsa_decrypt(Session& session, const Bytes& blob, EVP_PKEY* public_key, keymaster_padding_t padding) {
    if (!public_key) return false;
    UniqueContext context(EVP_PKEY_CTX_new(public_key, nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_encrypt_init(context.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_padding(context.get(), padding == KM_PAD_RSA_OAEP ? RSA_PKCS1_OAEP_PADDING : RSA_PKCS1_PADDING) != 1) return false;
    if (padding == KM_PAD_RSA_OAEP &&
        (EVP_PKEY_CTX_set_rsa_oaep_md(context.get(), EVP_sha256()) != 1 || EVP_PKEY_CTX_set_rsa_mgf1_md(context.get(), EVP_sha1()) != 1)) return false;
    Bytes plaintext = message(), ciphertext, recovered;
    size_t length = EVP_PKEY_size(public_key); ciphertext.resize(length);
    if (EVP_PKEY_encrypt(context.get(), ciphertext.data(), &length, plaintext.data(), plaintext.size()) != 1) return false;
    ciphertext.resize(length);
    return operate(session, hal::KeyPurpose::DECRYPT, blob, rsa_operation(padding), ciphertext, {}, &recovered) && equal(plaintext, recovered);
}
static bool pkcs8(Bytes* output) {
    std::unique_ptr<RSA, decltype(&RSA_free)> rsa(RSA_new(), RSA_free);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> exponent(BN_new(), BN_free);
    UniquePkey key(EVP_PKEY_new(), EVP_PKEY_free);
    if (!rsa || !exponent || !key || !BN_set_word(exponent.get(), 65537) ||
        !RSA_generate_key_ex(rsa.get(), 2048, exponent.get(), nullptr) || !EVP_PKEY_assign_RSA(key.get(), rsa.get())) return false;
    (void)rsa.release();
    std::unique_ptr<PKCS8_PRIV_KEY_INFO, decltype(&PKCS8_PRIV_KEY_INFO_free)> encoded(EVP_PKEY2PKCS8(key.get()), PKCS8_PRIV_KEY_INFO_free);
    if (!encoded) return false;
    int length = i2d_PKCS8_PRIV_KEY_INFO(encoded.get(), nullptr);
    if (length <= 0) return false;
    output->resize(length); uint8_t* cursor = output->data();
    return i2d_PKCS8_PRIV_KEY_INFO(encoded.get(), &cursor) == length;
}
static bool has(const Params& params, hal::Tag tag, uint64_t value) {
    for (const auto& param : params) if (param.tag == tag) {
        if (tag == hal::Tag::RSA_PUBLIC_EXPONENT) return param.f.longInteger == value;
        if (tag == hal::Tag::ALGORITHM) return static_cast<uint32_t>(param.f.algorithm) == value;
        return param.f.integer == value;
    }
    return false;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s /vendor/lib/hw/keystore.tuna.so\n", argv[0]); return 1; }
    Results results;
    std::unique_ptr<void, decltype(&dlclose)> library(dlopen(argv[1], RTLD_NOW | RTLD_LOCAL), dlclose);
    auto* module = library ? static_cast<hw_module_t*>(dlsym(library.get(), HAL_MODULE_INFO_SYM_AS_STR)) : nullptr;
    results.check("module-load", [&] { return module && module->methods && module->methods->open &&
        module->module_api_version == KEYMASTER_MODULE_API_VERSION_1_0; });
    if (results.failed) return 1;
    Session session(module);
    results.check("factory-open", [&] { return session.open(); });
    if (results.failed) return 1;
    Keys keys(&session);
    results.check("hardware-features", [&] {
        bool success = false;
        auto status = session.get()->getHardwareFeatures([&](bool secure, bool ec, bool symmetric, bool, bool,
            const android::hardware::hidl_string&, const android::hardware::hidl_string&) { success = secure && ec && symmetric; });
        return status.isOk() && success;
    });
    Bytes rsa_blob, imported_blob, ec_blob, aes_blob, hmac_blob;
    UniquePkey rsa_public(nullptr, EVP_PKEY_free), imported_public(nullptr, EVP_PKEY_free), ec_public(nullptr, EVP_PKEY_free);
    results.check("rsa-generate", [&] { return create(session, keys, rsa_params(), &rsa_blob); });
    results.check("rsa-characteristics", [&] {
        bool success = false;
        auto status = session.get()->getKeyCharacteristics(rsa_blob, {}, {}, [&](hal::ErrorCode error, const hal::KeyCharacteristics& characteristics) {
            success = error == hal::ErrorCode::OK && has(characteristics.teeEnforced, hal::Tag::ALGORITHM, KM_ALGORITHM_RSA) &&
                has(characteristics.teeEnforced, hal::Tag::KEY_SIZE, 2048) && has(characteristics.teeEnforced, hal::Tag::RSA_PUBLIC_EXPONENT, 65537);
        });
        return status.isOk() && success;
    });
    results.check("rsa-export-x509", [&] { rsa_public = public_key(session, rsa_blob); return rsa_public != nullptr; });
    results.check("rsa-pkcs1-sha256-sign-verify", [&] { return sign_verify(session, rsa_blob, rsa_public.get(), KM_PAD_RSA_PKCS1_1_5_SIGN); });
    results.check("rsa-pss-sha256-sign-verify", [&] { return sign_verify(session, rsa_blob, rsa_public.get(), KM_PAD_RSA_PSS); });
    results.check("rsa-pkcs1-decrypt", [&] { return rsa_decrypt(session, rsa_blob, rsa_public.get(), KM_PAD_RSA_PKCS1_1_5_ENCRYPT); });
    results.check("rsa-oaep-sha256-decrypt", [&] { return rsa_decrypt(session, rsa_blob, rsa_public.get(), KM_PAD_RSA_OAEP); });
    results.check("rsa-import-pkcs8", [&] { Bytes encoded; return pkcs8(&encoded) && create(session, keys, rsa_params(), &imported_blob, &encoded); });
    results.check("rsa-import-sign-verify", [&] { imported_public = public_key(session, imported_blob);
        return sign_verify(session, imported_blob, imported_public.get(), KM_PAD_RSA_PKCS1_1_5_SIGN); });
    results.check("ec-p256-generate", [&] {
        return create(session, keys, parameters({scalar(KM_TAG_ALGORITHM, KM_ALGORITHM_EC), scalar(KM_TAG_KEY_SIZE, 256),
            scalar(KM_TAG_EC_CURVE, KM_EC_CURVE_P_256), scalar(KM_TAG_PURPOSE, KM_PURPOSE_SIGN), scalar(KM_TAG_PURPOSE, KM_PURPOSE_VERIFY),
            scalar(KM_TAG_DIGEST, KM_DIGEST_SHA_2_256), scalar(KM_TAG_NO_AUTH_REQUIRED, 1)}), &ec_blob);
    });
    results.check("ec-sha256-sign-verify", [&] { ec_public = public_key(session, ec_blob);
        return sign_verify(session, ec_blob, ec_public.get(), KM_PAD_NONE, true); });
    results.check("aes256-gcm-generate", [&] {
        return create(session, keys, parameters({scalar(KM_TAG_ALGORITHM, KM_ALGORITHM_AES), scalar(KM_TAG_KEY_SIZE, 256),
            scalar(KM_TAG_PURPOSE, KM_PURPOSE_ENCRYPT), scalar(KM_TAG_PURPOSE, KM_PURPOSE_DECRYPT),
            scalar(KM_TAG_BLOCK_MODE, KM_MODE_GCM), scalar(KM_TAG_PADDING, KM_PAD_NONE),
            scalar(KM_TAG_MIN_MAC_LENGTH, 128), scalar(KM_TAG_NO_AUTH_REQUIRED, 1)}), &aes_blob);
    });
    Params aes_params = parameters({scalar(KM_TAG_BLOCK_MODE, KM_MODE_GCM), scalar(KM_TAG_PADDING, KM_PAD_NONE), scalar(KM_TAG_MAC_LENGTH, 128)});
    Params aes_decrypt_params;
    Bytes aes_ciphertext;
    results.check("aes-gcm-encrypt-decrypt", [&] {
        Params generated;
        Bytes recovered;
        if (!operate(session, hal::KeyPurpose::ENCRYPT, aes_blob, aes_params, message(), {}, &aes_ciphertext, &generated)) return false;
        bool nonce_found = false;
        aes_decrypt_params = aes_params;
        for (const auto& param : generated) if (param.tag == hal::Tag::NONCE) {
            size_t count = aes_decrypt_params.size(); aes_decrypt_params.resize(count + 1); aes_decrypt_params[count] = param;
            nonce_found = param.blob.size() == 12;
        }
        return nonce_found && aes_ciphertext.size() == message().size() + 16 &&
            operate(session, hal::KeyPurpose::DECRYPT, aes_blob, aes_decrypt_params, aes_ciphertext, {}, &recovered) && equal(recovered, message());
    });
    results.check("hmac-sha256-generate", [&] {
        return create(session, keys, parameters({scalar(KM_TAG_ALGORITHM, KM_ALGORITHM_HMAC), scalar(KM_TAG_KEY_SIZE, 256),
            scalar(KM_TAG_PURPOSE, KM_PURPOSE_SIGN), scalar(KM_TAG_PURPOSE, KM_PURPOSE_VERIFY),
            scalar(KM_TAG_DIGEST, KM_DIGEST_SHA_2_256), scalar(KM_TAG_MIN_MAC_LENGTH, 256), scalar(KM_TAG_NO_AUTH_REQUIRED, 1)}), &hmac_blob);
    });
    results.check("hmac-sha256-sign-verify", [&] {
        // HmacOperationFactory requires KM_TAG_MAC_LENGTH for SIGN and rejects it for VERIFY.
        Params sign_params = parameters({scalar(KM_TAG_DIGEST, KM_DIGEST_SHA_2_256), scalar(KM_TAG_MAC_LENGTH, 256)});
        Params verify_params = parameters({scalar(KM_TAG_DIGEST, KM_DIGEST_SHA_2_256)});
        Bytes signature, output;
        return operate(session, hal::KeyPurpose::SIGN, hmac_blob, sign_params, message(), {}, &signature) && signature.size() == 32 &&
            operate(session, hal::KeyPurpose::VERIFY, hmac_blob, verify_params, message(), signature, &output);
    });
    results.check("factory-close-reopen", [&] { session.close(); return session.open(); });
    results.check("reopened-rsa-sign-verify", [&] { return sign_verify(session, rsa_blob, rsa_public.get(), KM_PAD_RSA_PKCS1_1_5_SIGN); });
    results.check("reopened-aes-decrypt", [&] { Bytes recovered;
        return aes_ciphertext.size() && operate(session, hal::KeyPurpose::DECRYPT, aes_blob, aes_decrypt_params, aes_ciphertext, {}, &recovered) && equal(recovered, message()); });
    bool deleted_rsa = false;
    for (size_t index = 0; index < keys.blobs.size(); ++index) {
        char name[64]; snprintf(name, sizeof(name), "delete-key-%zu", index);
        results.check(name, [&] {
            if (!session.get() && !session.open()) return false;
            auto status = session.get()->deleteKey(keys.blobs[index]);
            bool success = status.isOk() && static_cast<hal::ErrorCode>(status) == hal::ErrorCode::OK;
            if (success && equal(keys.blobs[index], rsa_blob)) deleted_rsa = true;
            if (success) keys.blobs[index] = {};
            return success;
        });
    }
    keys.blobs.erase(std::remove_if(keys.blobs.begin(), keys.blobs.end(), [](const Bytes& blob) { return blob.size() == 0; }), keys.blobs.end());
    results.check("deleted-rsa-begin-rejected", [&] {
        if (!session.get() || !deleted_rsa) return false;
        bool rejected = false; uint64_t handle = 0;
        auto status = session.get()->begin(hal::KeyPurpose::SIGN, rsa_blob, rsa_operation(KM_PAD_RSA_PKCS1_1_5_SIGN),
            [&](hal::ErrorCode error, const Params&, uint64_t created) { rejected = error == hal::ErrorCode::INVALID_KEY_BLOB; handle = created; });
        if (handle) (void)session.get()->abort(handle);
        return status.isOk() && rejected;
    });
    return results.failed ? 1 : 0;
}
