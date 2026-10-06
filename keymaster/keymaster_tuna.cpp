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
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <hardware/keymaster1.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/engine.h>
#include <keymaster/soft_keymaster_device.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <cryptoki.h>
#include <pkcs11.h>
#include <memory>
#include <new>
#include <mutex>

using keystore_module_t = struct keystore_module;

namespace {
constexpr size_t ID_LENGTH = 32;
constexpr uint32_t KEY_VERSION = 2;
constexpr size_t HEADER_LENGTH = 56;
constexpr size_t MAX_BLOB_LENGTH = 65536;
constexpr size_t MAX_RSA_BYTES = 512;
constexpr size_t MAX_OPERATIONS = 16;
using Unique_RSA = std::unique_ptr<RSA, decltype(&RSA_free)>;
using Unique_EVP_PKEY = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Unique_PKCS8 = std::unique_ptr<PKCS8_PRIV_KEY_INFO, decltype(&PKCS8_PRIV_KEY_INFO_free)>;

class ByteArray {
public:
    explicit ByteArray(size_t length) : data_(static_cast<CK_BYTE*>(malloc(length))), length_(length) {}
    ~ByteArray() { free(data_); }
    CK_BYTE* get() const { return data_; }
    size_t length() const { return length_; }
    ByteArray(const ByteArray&) = delete;
    ByteArray& operator=(const ByteArray&) = delete;
private:
    CK_BYTE* data_;
    size_t length_;
};
using Unique_ByteArray = std::unique_ptr<ByteArray>;

class CryptoSession {
public:
    explicit CryptoSession(CK_SESSION_HANDLE primary) : primary_(primary), session_(primary) {
        if (C_OpenSession(CKV_TOKEN_USER, CKF_SERIAL_SESSION | CKF_RW_SESSION |
                          CKVF_OPEN_SUB_SESSION, nullptr, nullptr, &session_) != CKR_OK)
            session_ = CK_INVALID_HANDLE;
    }
    ~CryptoSession() { if (session_ != CK_INVALID_HANDLE) C_CloseSession(session_); }
    CK_SESSION_HANDLE get() const { return session_; }
    CK_SESSION_HANDLE getPrimary() const { return primary_; }
private:
    CK_SESSION_HANDLE primary_;
    CK_SESSION_HANDLE session_;
};

class ObjectHandle {
public:
    explicit ObjectHandle(const CryptoSession* session, CK_OBJECT_HANDLE handle = CK_INVALID_HANDLE)
        : session_(session), handle_(handle) {}
    ~ObjectHandle() { reset(CK_INVALID_HANDLE); }
    CK_OBJECT_HANDLE get() const { return handle_; }
    void reset(CK_OBJECT_HANDLE handle) {
        if (handle_ != CK_INVALID_HANDLE) C_CloseObjectHandle(session_->getPrimary(), handle_);
        handle_ = handle;
    }
private:
    const CryptoSession* session_;
    CK_OBJECT_HANDLE handle_;
};

static ByteArray* bignum_to_array(const BIGNUM* number) {
    if (!number) return nullptr;
    Unique_ByteArray bytes(new (std::nothrow) ByteArray(BN_num_bytes(number)));
    if (!bytes || !bytes->get() || BN_bn2bin(number, bytes->get()) != BN_num_bytes(number))
        return nullptr;
    return bytes.release();
}

static int find_single_object(const uint8_t* identifier, size_t length, CK_OBJECT_CLASS object_class,
                              const CryptoSession* session, ObjectHandle* object) {
    CK_ATTRIBUTE attributes[] = {
        {CKA_ID, const_cast<uint8_t*>(identifier), static_cast<CK_ULONG>(length)},
        {CKA_CLASS, &object_class, sizeof(object_class)},
    };
    if (C_FindObjectsInit(session->get(), attributes, 2) != CKR_OK) return -1;
    CK_OBJECT_HANDLE handles[2] = {};
    CK_ULONG count = 0;
    CK_RV result = C_FindObjects(session->get(), handles, 2, &count);
    CK_RV final_result = C_FindObjectsFinal(session->get());
    if (result != CKR_OK || final_result != CKR_OK || count != 1) {
        for (CK_ULONG index = 0; index < count && index < 2; ++index)
            C_CloseObjectHandle(session->getPrimary(), handles[index]);
        return -1;
    }
    object->reset(handles[0]);
    return 0;
}

static void write_integer(uint8_t* destination, uint64_t value, size_t length) {
    for (size_t index = 0; index < length; ++index)
        destination[length - index - 1] = static_cast<uint8_t>(value >> (index * 8));
}
static uint64_t read_integer(const uint8_t* source, size_t length) {
    uint64_t value = 0;
    for (size_t index = 0; index < length; ++index) value = (value << 8) | source[index];
    return value;
}
static bool hardware_tag(keymaster_tag_t tag) {
    return tag == KM_TAG_ALGORITHM || tag == KM_TAG_KEY_SIZE ||
           tag == KM_TAG_RSA_PUBLIC_EXPONENT || tag == KM_TAG_ORIGIN;
}
static bool hidden_tag(keymaster_tag_t tag) {
    return tag == KM_TAG_APPLICATION_ID || tag == KM_TAG_APPLICATION_DATA ||
           tag == KM_TAG_ROOT_OF_TRUST;
}
static const keymaster_key_param_t* parameter(const keymaster_key_param_set_t* params,
                                             keymaster_tag_t tag) {
    if (!params || (params->length && !params->params)) return nullptr;
    const keymaster_key_param_t* found = nullptr;
    for (size_t index = 0; index < params->length; ++index) {
        if (params->params[index].tag == tag) {
            if (found) return nullptr;
            found = &params->params[index];
        }
    }
    return found;
}
static bool valid_size(uint32_t bits) { return bits >= 512 && bits <= 4096 && bits % 8 == 0; }
static bool valid_exponent(uint64_t exponent) { return exponent >= 3 && (exponent & 1); }

// The tail retains software authorizations; hardware characteristics expose only RSA properties.
static keymaster_error_t keyblob_save(const uint8_t* identifier, uint32_t bits, uint64_t exponent,
                                      keymaster_key_origin_t origin,
                                      const keymaster_key_param_set_t* params,
                                      keymaster_key_blob_t* blob) {
    size_t length = HEADER_LENGTH;
    if (!params || (params->length && !params->params)) return KM_ERROR_INVALID_ARGUMENT;
    for (size_t index = 0; index < params->length; ++index) {
        const auto& entry = params->params[index];
        keymaster_tag_type_t type = keymaster_tag_get_type(entry.tag);
        size_t payload = (type == KM_BYTES || type == KM_BIGNUM) ? entry.blob.data_length : 8;
        if (payload > MAX_BLOB_LENGTH - 8 || length > MAX_BLOB_LENGTH - payload - 8 ||
            ((type == KM_BYTES || type == KM_BIGNUM) && payload && !entry.blob.data))
            return KM_ERROR_INVALID_ARGUMENT;
        length += 8 + payload;
    }
    uint8_t* bytes = static_cast<uint8_t*>(calloc(1, length));
    if (!bytes) return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    write_integer(bytes, KEY_VERSION, 4);
    memcpy(bytes + 4, identifier, ID_LENGTH);
    write_integer(bytes + 36, bits, 4);
    write_integer(bytes + 40, exponent, 8);
    write_integer(bytes + 48, origin, 4);
    write_integer(bytes + 52, params->length, 4);
    size_t offset = HEADER_LENGTH;
    for (size_t index = 0; index < params->length; ++index) {
        const auto& entry = params->params[index];
        keymaster_tag_type_t type = keymaster_tag_get_type(entry.tag);
        bool is_blob = type == KM_BYTES || type == KM_BIGNUM;
        size_t payload = is_blob ? entry.blob.data_length : 8;
        write_integer(bytes + offset, entry.tag, 4);
        write_integer(bytes + offset + 4, payload, 4);
        if (is_blob) {
            if (payload) memcpy(bytes + offset + 8, entry.blob.data, payload);
        } else {
            uint64_t value = 0;
            switch (type) {
            case KM_ENUM: case KM_ENUM_REP: value = entry.enumerated; break;
            case KM_UINT: case KM_UINT_REP: value = entry.integer; break;
            case KM_ULONG: case KM_ULONG_REP: value = entry.long_integer; break;
            case KM_DATE: value = entry.date_time; break;
            case KM_BOOL: value = entry.boolean; break;
            default: free(bytes); return KM_ERROR_INVALID_TAG;
            }
            write_integer(bytes + offset + 8, value, 8);
        }
        offset += payload + 8;
    }
    *blob = {bytes, length};
    return KM_ERROR_OK;
}

static keymaster_error_t validate_blob(const keymaster_key_blob_t* blob) {
    if (!blob || !blob->key_material || blob->key_material_size < HEADER_LENGTH ||
        blob->key_material_size > MAX_BLOB_LENGTH) return KM_ERROR_INVALID_KEY_BLOB;
    const uint8_t* bytes = blob->key_material;
    if (read_integer(bytes, 4) != KEY_VERSION || !valid_size(read_integer(bytes + 36, 4)) ||
        !valid_exponent(read_integer(bytes + 40, 8)) ||
        (read_integer(bytes + 48, 4) != KM_ORIGIN_GENERATED &&
         read_integer(bytes + 48, 4) != KM_ORIGIN_IMPORTED)) return KM_ERROR_INVALID_KEY_BLOB;
    size_t offset = HEADER_LENGTH;
    size_t count = read_integer(bytes + 52, 4);
    for (size_t index = 0; index < count; ++index) {
        if (blob->key_material_size - offset < 8) return KM_ERROR_INVALID_KEY_BLOB;
        auto tag = static_cast<keymaster_tag_t>(read_integer(bytes + offset, 4));
        auto type = keymaster_tag_get_type(tag);
        size_t payload = read_integer(bytes + offset + 4, 4);
        bool scalar = type == KM_ENUM || type == KM_ENUM_REP || type == KM_UINT ||
                      type == KM_UINT_REP || type == KM_ULONG || type == KM_ULONG_REP ||
                      type == KM_DATE || type == KM_BOOL;
        if ((!scalar && type != KM_BYTES && type != KM_BIGNUM) ||
            (scalar && payload != 8) || payload > blob->key_material_size - offset - 8)
            return KM_ERROR_INVALID_KEY_BLOB;
        offset += payload + 8;
    }
    return offset == blob->key_material_size ? KM_ERROR_OK : KM_ERROR_INVALID_KEY_BLOB;
}
static int keyblob_restore(const CryptoSession* session, const keymaster_key_blob_t* blob,
                           ObjectHandle* public_key, ObjectHandle* private_key) {
    if (validate_blob(blob) != KM_ERROR_OK) return -1;
    return find_single_object(blob->key_material + 4, ID_LENGTH, CKO_PUBLIC_KEY, session, public_key)
        || find_single_object(blob->key_material + 4, ID_LENGTH, CKO_PRIVATE_KEY, session, private_key);
}

static bool application_matches(const keymaster_key_blob_t* blob, keymaster_tag_t wanted,
                                const keymaster_blob_t* supplied) {
    size_t offset = HEADER_LENGTH;
    size_t count = read_integer(blob->key_material + 52, 4);
    for (size_t index = 0; index < count; ++index) {
        auto tag = static_cast<keymaster_tag_t>(read_integer(blob->key_material + offset, 4));
        size_t length = read_integer(blob->key_material + offset + 4, 4);
        if (tag == wanted && (length != (supplied ? supplied->data_length : 0) ||
            (length && (!supplied->data || memcmp(blob->key_material + offset + 8, supplied->data, length)))))
            return false;
        offset += length + 8;
    }
    return true;
}
static bool authorized(const keymaster_key_blob_t* blob, keymaster_tag_t wanted, uint32_t value) {
    size_t offset = HEADER_LENGTH;
    size_t count = read_integer(blob->key_material + 52, 4);
    for (size_t index = 0; index < count; ++index) {
        auto tag = static_cast<keymaster_tag_t>(read_integer(blob->key_material + offset, 4));
        size_t length = read_integer(blob->key_material + offset + 4, 4);
        if (tag == wanted && read_integer(blob->key_material + offset + 8, 8) == value)
            return true;
        offset += length + 8;
    }
    return false;
}

static keymaster_error_t characteristics(const keymaster_key_blob_t* blob,
                                         keymaster_key_characteristics_t** output) {
    if (!output) return KM_ERROR_OUTPUT_PARAMETER_NULL;
    *output = nullptr;
    auto error = validate_blob(blob);
    if (error != KM_ERROR_OK) return error;
    auto* result = static_cast<keymaster_key_characteristics_t*>(calloc(1, sizeof(keymaster_key_characteristics_t)));
    if (!result) return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    result->hw_enforced.params = static_cast<keymaster_key_param_t*>(calloc(4, sizeof(keymaster_key_param_t)));
    size_t count = read_integer(blob->key_material + 52, 4);
    result->sw_enforced.params = static_cast<keymaster_key_param_t*>(calloc(count ? count : 1, sizeof(keymaster_key_param_t)));
    if (!result->hw_enforced.params || !result->sw_enforced.params) {
        keymaster_free_characteristics(result); free(result); return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    }
    result->hw_enforced.length = 4;
    result->hw_enforced.params[0] = keymaster_param_enum(KM_TAG_ALGORITHM, KM_ALGORITHM_RSA);
    result->hw_enforced.params[1] = keymaster_param_int(KM_TAG_KEY_SIZE, read_integer(blob->key_material + 36, 4));
    result->hw_enforced.params[2] = keymaster_param_long(KM_TAG_RSA_PUBLIC_EXPONENT, read_integer(blob->key_material + 40, 8));
    result->hw_enforced.params[3] = keymaster_param_enum(KM_TAG_ORIGIN, read_integer(blob->key_material + 48, 4));
    size_t offset = HEADER_LENGTH;
    for (size_t index = 0; index < count; ++index) {
        auto tag = static_cast<keymaster_tag_t>(read_integer(blob->key_material + offset, 4));
        size_t payload = read_integer(blob->key_material + offset + 4, 4);
        const uint8_t* value = blob->key_material + offset + 8;
        offset += payload + 8;
        if (hardware_tag(tag) || hidden_tag(tag)) continue;
        auto& entry = result->sw_enforced.params[result->sw_enforced.length++];
        entry.tag = tag;
        switch (keymaster_tag_get_type(tag)) {
        case KM_ENUM: case KM_ENUM_REP: entry.enumerated = read_integer(value, 8); break;
        case KM_UINT: case KM_UINT_REP: entry.integer = read_integer(value, 8); break;
        case KM_ULONG: case KM_ULONG_REP: entry.long_integer = read_integer(value, 8); break;
        case KM_DATE: entry.date_time = read_integer(value, 8); break;
        case KM_BOOL: entry.boolean = read_integer(value, 8) != 0; break;
        case KM_BYTES: case KM_BIGNUM: {
            auto* copy = static_cast<uint8_t*>(malloc(payload ? payload : 1));
            if (!copy) { keymaster_free_characteristics(result); free(result); return KM_ERROR_MEMORY_ALLOCATION_FAILED; }
            if (payload) memcpy(copy, value, payload);
            entry.blob = {copy, payload};
            break;
        }
        default: keymaster_free_characteristics(result); free(result); return KM_ERROR_INVALID_KEY_BLOB;
        }
    }
    *output = result;
    return KM_ERROR_OK;
}

struct Operation {
    uint64_t handle;
    keymaster_key_blob_t blob;
    size_t used;
    uint8_t input[MAX_RSA_BYTES];
    keymaster_purpose_t purpose;
    keymaster_padding_t padding;
    const EVP_MD* digest;
    EVP_MD_CTX* digest_context;
    EVP_PKEY* public_key;
    CK_SESSION_HANDLE primary;
};
struct Device {
    keymaster1_device_t api;
    CK_SESSION_HANDLE primary;
    Operation operations[MAX_OPERATIONS];
};
static pthread_mutex_t module_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool module_open = false;
class Lock {
public:
    Lock() { pthread_mutex_lock(&module_mutex); }
    ~Lock() { pthread_mutex_unlock(&module_mutex); }
};
static Device* device(const keymaster1_device_t* api) { return static_cast<Device*>(api->context); }
static void clear_operation(Operation* operation) {
    EVP_MD_CTX_free(operation->digest_context);
    EVP_PKEY_free(operation->public_key);
    free(const_cast<uint8_t*>(operation->blob.key_material));
    memset(operation, 0, sizeof(*operation));
}
static Operation* operation_for(Device* dev, uint64_t handle) {
    for (auto& operation : dev->operations)
        if (handle && operation.handle == handle) return &operation;
    return nullptr;
}

template <typename Value>
static keymaster_error_t capability(Value value, bool supported, Value** values, size_t* count) {
    if (!values || !count) return KM_ERROR_OUTPUT_PARAMETER_NULL;
    *values = nullptr; *count = 0;
    // map_digests constructs a range even for empty lists, so allocate an empty-list sentinel.
    auto* array = static_cast<Value*>(malloc(sizeof(Value)));
    if (!array) return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    array[0] = value;
    *values = array; *count = supported ? 1 : 0;
    return KM_ERROR_OK;
}
static keymaster_error_t algorithms(const keymaster1_device_t*, keymaster_algorithm_t** values, size_t* count) {
    return capability(KM_ALGORITHM_RSA, true, values, count);
}
static keymaster_error_t block_modes(const keymaster1_device_t*, keymaster_algorithm_t,
                                     keymaster_purpose_t, keymaster_block_mode_t** values, size_t* count) {
    return capability(KM_MODE_ECB, false, values, count);
}
static keymaster_error_t paddings(const keymaster1_device_t*, keymaster_algorithm_t algorithm,
                                  keymaster_purpose_t purpose, keymaster_padding_t** values, size_t* count) {
    return capability(KM_PAD_NONE, algorithm == KM_ALGORITHM_RSA && purpose == KM_PURPOSE_SIGN, values, count);
}
static keymaster_error_t digests(const keymaster1_device_t*, keymaster_algorithm_t algorithm,
                                 keymaster_purpose_t purpose, keymaster_digest_t** values, size_t* count) {
    return capability(KM_DIGEST_NONE, algorithm == KM_ALGORITHM_RSA && purpose == KM_PURPOSE_SIGN, values, count);
}
static keymaster_error_t import_formats(const keymaster1_device_t*, keymaster_algorithm_t algorithm,
                                        keymaster_key_format_t** values, size_t* count) {
    return capability(KM_KEY_FORMAT_PKCS8, algorithm == KM_ALGORITHM_RSA, values, count);
}
static keymaster_error_t export_formats(const keymaster1_device_t*, keymaster_algorithm_t algorithm,
                                        keymaster_key_format_t** values, size_t* count) {
    return capability(KM_KEY_FORMAT_X509, algorithm == KM_ALGORITHM_RSA, values, count);
}
static keymaster_error_t get_characteristics(const keymaster1_device_t*, const keymaster_key_blob_t* blob,
                                            const keymaster_blob_t* client_id, const keymaster_blob_t* app_data,
                                            keymaster_key_characteristics_t** output) {
    if (output) *output = nullptr;
    if (validate_blob(blob) != KM_ERROR_OK ||
        !application_matches(blob, KM_TAG_APPLICATION_ID, client_id) ||
        !application_matches(blob, KM_TAG_APPLICATION_DATA, app_data)) return KM_ERROR_INVALID_KEY_BLOB;
    return characteristics(blob, output);
}
static keymaster_error_t entropy(const keymaster1_device_t* api, const uint8_t* data, size_t length) {
    if ((length && !data) || length > UINT32_MAX) return KM_ERROR_INVALID_ARGUMENT;
    Lock lock;
    CryptoSession session(device(api)->primary);
    if (session.get() == CK_INVALID_HANDLE) return KM_ERROR_SECURE_HW_COMMUNICATION_FAILED;
    return C_SeedRandom(session.get(), data, length) == CKR_OK ? KM_ERROR_OK : KM_ERROR_UNKNOWN_ERROR;
}

static keymaster_error_t publish_key(const uint8_t* identifier, uint32_t bits, uint64_t exponent,
                                     keymaster_key_origin_t origin, const keymaster_key_param_set_t* params,
                                     keymaster_key_blob_t* blob, keymaster_key_characteristics_t** output,
                                     const CryptoSession& session, CK_OBJECT_HANDLE public_key,
                                     CK_OBJECT_HANDLE private_key) {
    auto error = keyblob_save(identifier, bits, exponent, origin, params, blob);
    if (error == KM_ERROR_OK && output) error = characteristics(blob, output);
    if (error != KM_ERROR_OK) {
        free(const_cast<uint8_t*>(blob->key_material)); *blob = {};
        C_DestroyObject(session.get(), private_key);
        C_DestroyObject(session.get(), public_key);
    }
    return error;
}
static keymaster_error_t generate(const keymaster1_device_t* api, const keymaster_key_param_set_t* params,
                                  keymaster_key_blob_t* blob, keymaster_key_characteristics_t** output) {
    if (!blob) return KM_ERROR_OUTPUT_PARAMETER_NULL;
    *blob = {}; if (output) *output = nullptr;
    const auto* algorithm = parameter(params, KM_TAG_ALGORITHM);
    const auto* size = parameter(params, KM_TAG_KEY_SIZE);
    const auto* exponent = parameter(params, KM_TAG_RSA_PUBLIC_EXPONENT);
    if (!algorithm || algorithm->enumerated != KM_ALGORITHM_RSA) return KM_ERROR_UNSUPPORTED_ALGORITHM;
    if (!size || !valid_size(size->integer)) return KM_ERROR_UNSUPPORTED_KEY_SIZE;
    uint64_t public_exponent = exponent ? exponent->long_integer : 65537;
    if (!valid_exponent(public_exponent)) return KM_ERROR_INVALID_ARGUMENT;
    uint8_t identifier[ID_LENGTH], exponent_bytes[8];
    if (RAND_bytes(identifier, sizeof(identifier)) != 1) return KM_ERROR_UNKNOWN_ERROR;
    write_integer(exponent_bytes, public_exponent, sizeof(exponent_bytes));
    CK_BBOOL enabled = CK_TRUE, disabled = CK_FALSE;
    CK_ULONG bits = size->integer;
    CK_ATTRIBUTE public_template[] = {
        {CKA_ID, identifier, sizeof(identifier)}, {CKA_TOKEN, &enabled, sizeof(enabled)},
        {CKA_ENCRYPT, &enabled, sizeof(enabled)}, {CKA_VERIFY, &enabled, sizeof(enabled)},
        {CKA_MODULUS_BITS, &bits, sizeof(bits)}, {CKA_PUBLIC_EXPONENT, exponent_bytes, sizeof(exponent_bytes)},
    };
    CK_ATTRIBUTE private_template[] = {
        {CKA_ID, identifier, sizeof(identifier)}, {CKA_TOKEN, &enabled, sizeof(enabled)},
        {CKA_DECRYPT, &disabled, sizeof(disabled)}, {CKA_SIGN, &enabled, sizeof(enabled)},
    };
    Lock lock;
    CryptoSession session(device(api)->primary);
    if (session.get() == CK_INVALID_HANDLE) return KM_ERROR_SECURE_HW_COMMUNICATION_FAILED;
    CK_MECHANISM mechanism = {CKM_RSA_PKCS_KEY_PAIR_GEN, nullptr, 0};
    CK_OBJECT_HANDLE public_handle = CK_INVALID_HANDLE, private_handle = CK_INVALID_HANDLE;
    if (C_GenerateKeyPair(session.get(), &mechanism, public_template, 6, private_template, 4,
                          &public_handle, &private_handle) != CKR_OK) return KM_ERROR_UNKNOWN_ERROR;
    ObjectHandle public_key(&session, public_handle), private_key(&session, private_handle);
    return publish_key(identifier, bits, public_exponent, KM_ORIGIN_GENERATED, params, blob, output,
                       session, public_handle, private_handle);
}

static keymaster_error_t import_key(const keymaster1_device_t* api, const keymaster_key_param_set_t* params,
                                    keymaster_key_format_t format, const keymaster_blob_t* input,
                                    keymaster_key_blob_t* blob, keymaster_key_characteristics_t** output) {
    if (!blob) return KM_ERROR_OUTPUT_PARAMETER_NULL;
    *blob = {}; if (output) *output = nullptr;
    if (format != KM_KEY_FORMAT_PKCS8) return KM_ERROR_UNSUPPORTED_KEY_FORMAT;
    const auto* algorithm = parameter(params, KM_TAG_ALGORITHM);
    if (!algorithm || algorithm->enumerated != KM_ALGORITHM_RSA) return KM_ERROR_UNSUPPORTED_ALGORITHM;
    if (!input || !input->data || input->data_length > LONG_MAX) return KM_ERROR_INVALID_ARGUMENT;
    const uint8_t* cursor = input->data;
    Unique_PKCS8 pkcs8(d2i_PKCS8_PRIV_KEY_INFO(nullptr, &cursor, input->data_length), PKCS8_PRIV_KEY_INFO_free);
    if (!pkcs8 || cursor != input->data + input->data_length) return KM_ERROR_INVALID_ARGUMENT;
    Unique_EVP_PKEY pkey(EVP_PKCS82PKEY(pkcs8.get()), EVP_PKEY_free);
    if (!pkey || EVP_PKEY_id(pkey.get()) != EVP_PKEY_RSA) return KM_ERROR_UNSUPPORTED_ALGORITHM;
    Unique_RSA rsa(EVP_PKEY_get1_RSA(pkey.get()), RSA_free);
    if (!rsa || RSA_check_key(rsa.get()) != 1) return KM_ERROR_INVALID_ARGUMENT;
    uint32_t bits = BN_num_bits(rsa->n);
    if (!valid_size(bits) || BN_num_bits(rsa->e) > 64) return KM_ERROR_UNSUPPORTED_KEY_SIZE;
    uint8_t exponent_bytes[8] = {};
    int exponent_length = BN_num_bytes(rsa->e);
    BN_bn2bin(rsa->e, exponent_bytes + 8 - exponent_length);
    uint64_t exponent = read_integer(exponent_bytes, 8);
    if (!valid_exponent(exponent)) return KM_ERROR_INVALID_ARGUMENT;
    const auto* requested_size = parameter(params, KM_TAG_KEY_SIZE);
    const auto* requested_exponent = parameter(params, KM_TAG_RSA_PUBLIC_EXPONENT);
    if ((requested_size && requested_size->integer != bits) ||
        (requested_exponent && requested_exponent->long_integer != exponent)) return KM_ERROR_IMPORT_PARAMETER_MISMATCH;
    const BIGNUM* numbers[] = {rsa->n, rsa->e, rsa->d, rsa->p, rsa->q, rsa->dmp1, rsa->dmq1, rsa->iqmp};
    CK_ATTRIBUTE_TYPE types[] = {CKA_MODULUS, CKA_PUBLIC_EXPONENT, CKA_PRIVATE_EXPONENT,
        CKA_PRIME_1, CKA_PRIME_2, CKA_EXPONENT_1, CKA_EXPONENT_2, CKA_COEFFICIENT};
    Unique_ByteArray converted[8];
    for (size_t index = 0; index < 8; ++index) {
        converted[index].reset(bignum_to_array(numbers[index]));
        if (!converted[index]) return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    }
    uint8_t identifier[ID_LENGTH];
    if (RAND_bytes(identifier, sizeof(identifier)) != 1) return KM_ERROR_UNKNOWN_ERROR;
    CK_BBOOL enabled = CK_TRUE, disabled = CK_FALSE;
    CK_KEY_TYPE key_type = CKK_RSA;
    CK_OBJECT_CLASS public_class = CKO_PUBLIC_KEY, private_class = CKO_PRIVATE_KEY;
    CK_ATTRIBUTE public_template[] = {
        {CKA_ID, identifier, sizeof(identifier)}, {CKA_TOKEN, &enabled, sizeof(enabled)},
        {CKA_CLASS, &public_class, sizeof(public_class)}, {CKA_KEY_TYPE, &key_type, sizeof(key_type)},
        {CKA_ENCRYPT, &enabled, sizeof(enabled)}, {CKA_VERIFY, &enabled, sizeof(enabled)},
        {CKA_MODULUS, converted[0]->get(), static_cast<CK_ULONG>(converted[0]->length())},
        {CKA_PUBLIC_EXPONENT, converted[1]->get(), static_cast<CK_ULONG>(converted[1]->length())},
    };
    CK_ATTRIBUTE private_template[14] = {
        {CKA_ID, identifier, sizeof(identifier)}, {CKA_TOKEN, &enabled, sizeof(enabled)},
        {CKA_CLASS, &private_class, sizeof(private_class)}, {CKA_KEY_TYPE, &key_type, sizeof(key_type)},
        {CKA_DECRYPT, &disabled, sizeof(disabled)}, {CKA_SIGN, &enabled, sizeof(enabled)},
    };
    for (size_t index = 0; index < 8; ++index)
        private_template[index + 6] = {types[index], converted[index]->get(), static_cast<CK_ULONG>(converted[index]->length())};
    Lock lock;
    CryptoSession session(device(api)->primary);
    if (session.get() == CK_INVALID_HANDLE) return KM_ERROR_SECURE_HW_COMMUNICATION_FAILED;
    CK_OBJECT_HANDLE public_handle = CK_INVALID_HANDLE, private_handle = CK_INVALID_HANDLE;
    if (C_CreateObject(session.get(), public_template, 8, &public_handle) != CKR_OK) return KM_ERROR_UNKNOWN_ERROR;
    ObjectHandle public_key(&session, public_handle);
    if (C_CreateObject(session.get(), private_template, 14, &private_handle) != CKR_OK) {
        C_DestroyObject(session.get(), public_handle); return KM_ERROR_UNKNOWN_ERROR;
    }
    ObjectHandle private_key(&session, private_handle);
    return publish_key(identifier, bits, exponent, KM_ORIGIN_IMPORTED, params, blob, output,
                       session, public_handle, private_handle);
}

static keymaster_error_t export_key(const keymaster1_device_t* api, keymaster_key_format_t format,
                                    const keymaster_key_blob_t* blob, const keymaster_blob_t* client_id,
                                    const keymaster_blob_t* app_data, keymaster_blob_t* output) {
    if (!output) return KM_ERROR_OUTPUT_PARAMETER_NULL;
    *output = {};
    if (format != KM_KEY_FORMAT_X509) return KM_ERROR_UNSUPPORTED_KEY_FORMAT;
    if (validate_blob(blob) != KM_ERROR_OK ||
        !application_matches(blob, KM_TAG_APPLICATION_ID, client_id) ||
        !application_matches(blob, KM_TAG_APPLICATION_DATA, app_data)) return KM_ERROR_INVALID_KEY_BLOB;
    Lock lock;
    CryptoSession session(device(api)->primary);
    if (session.get() == CK_INVALID_HANDLE) return KM_ERROR_SECURE_HW_COMMUNICATION_FAILED;
    ObjectHandle public_key(&session), private_key(&session);
    if (keyblob_restore(&session, blob, &public_key, &private_key)) return KM_ERROR_INVALID_KEY_BLOB;
    CK_ATTRIBUTE attributes[] = {{CKA_MODULUS, nullptr, 0}, {CKA_PUBLIC_EXPONENT, nullptr, 0}};
    if (C_GetAttributeValue(session.get(), public_key.get(), attributes, 2) != CKR_OK ||
        !attributes[0].ulValueLen || attributes[0].ulValueLen > MAX_RSA_BYTES ||
        !attributes[1].ulValueLen || attributes[1].ulValueLen > 8) return KM_ERROR_UNKNOWN_ERROR;
    ByteArray modulus(attributes[0].ulValueLen), exponent(attributes[1].ulValueLen);
    if (!modulus.get() || !exponent.get()) return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    attributes[0].pValue = modulus.get(); attributes[1].pValue = exponent.get();
    if (C_GetAttributeValue(session.get(), public_key.get(), attributes, 2) != CKR_OK) return KM_ERROR_UNKNOWN_ERROR;
    Unique_RSA rsa(RSA_new(), RSA_free);
    Unique_EVP_PKEY pkey(EVP_PKEY_new(), EVP_PKEY_free);
    if (!rsa || !pkey) return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    rsa->n = BN_bin2bn(modulus.get(), modulus.length(), nullptr);
    rsa->e = BN_bin2bn(exponent.get(), exponent.length(), nullptr);
    if (!rsa->n || !rsa->e) return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    if (EVP_PKEY_assign_RSA(pkey.get(), rsa.get()) != 1) return KM_ERROR_UNKNOWN_ERROR;
    (void)rsa.release();
    int length = i2d_PUBKEY(pkey.get(), nullptr);
    if (length <= 0) return KM_ERROR_UNKNOWN_ERROR;
    auto* bytes = static_cast<uint8_t*>(malloc(length));
    if (!bytes) return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    uint8_t* cursor = bytes;
    if (i2d_PUBKEY(pkey.get(), &cursor) != length) { free(bytes); return KM_ERROR_UNKNOWN_ERROR; }
    *output = {bytes, static_cast<size_t>(length)};
    return KM_ERROR_OK;
}

static keymaster_error_t delete_key(const keymaster1_device_t* api, const keymaster_key_blob_t* blob) {
    if (validate_blob(blob) != KM_ERROR_OK) return KM_ERROR_INVALID_KEY_BLOB;
    Lock lock;
    Device* dev = device(api);
    for (auto& operation : dev->operations)
        if (operation.handle && memcmp(operation.blob.key_material + 4, blob->key_material + 4, ID_LENGTH) == 0)
            clear_operation(&operation);
    CryptoSession session(dev->primary);
    if (session.get() == CK_INVALID_HANDLE) return KM_ERROR_SECURE_HW_COMMUNICATION_FAILED;
    ObjectHandle public_key(&session), private_key(&session);
    bool found_public = find_single_object(blob->key_material + 4, ID_LENGTH, CKO_PUBLIC_KEY, &session, &public_key) == 0;
    bool found_private = find_single_object(blob->key_material + 4, ID_LENGTH, CKO_PRIVATE_KEY, &session, &private_key) == 0;
    CK_RV private_result = found_private ? C_DestroyObject(session.get(), private_key.get()) : CKR_OK;
    CK_RV public_result = found_public ? C_DestroyObject(session.get(), public_key.get()) : CKR_OK;
    if (private_result != CKR_OK || public_result != CKR_OK) return KM_ERROR_UNKNOWN_ERROR;
    return found_public || found_private ? KM_ERROR_OK : KM_ERROR_INVALID_KEY_BLOB;
}
static keymaster_error_t delete_all(const keymaster1_device_t*) { return KM_ERROR_UNIMPLEMENTED; }

static const EVP_MD* rsa_digest(uint32_t digest) {
    switch (digest) {
    case KM_DIGEST_MD5: return EVP_md5();
    case KM_DIGEST_SHA1: return EVP_sha1();
    case KM_DIGEST_SHA_2_224: return EVP_sha224();
    case KM_DIGEST_SHA_2_256: return EVP_sha256();
    case KM_DIGEST_SHA_2_384: return EVP_sha384();
    case KM_DIGEST_SHA_2_512: return EVP_sha512();
    default: return nullptr;
    }
}
static int rsa_operation_index = -1;
// C_Sign with CKM_RSA_X_509 is the raw RSA private transform for both signing and
// decryption; BoringSSL applies and removes padding around this callback.
static int private_transform(RSA* rsa, uint8_t* output, const uint8_t* input, size_t length) {
    auto* operation = static_cast<Operation*>(RSA_get_ex_data(rsa, rsa_operation_index));
    if (!operation || length != static_cast<size_t>(RSA_size(rsa)) || length > MAX_RSA_BYTES) return 0;
    uint8_t modulus[MAX_RSA_BYTES] = {};
    if (BN_bn2bin_padded(modulus, length, rsa->n) != 1 || memcmp(input, modulus, length) >= 0) return 0;
    CryptoSession session(operation->primary);
    ObjectHandle public_key(&session), private_key(&session);
    if (keyblob_restore(&session, &operation->blob, &public_key, &private_key)) return 0;
    CK_MECHANISM mechanism = {CKM_RSA_X_509, nullptr, 0};
    CK_ULONG result_length = length;
    return C_SignInit(session.get(), &mechanism, private_key.get()) == CKR_OK &&
           C_Sign(session.get(), const_cast<uint8_t*>(input), length, output, &result_length) == CKR_OK &&
           result_length == length;
}
static keymaster_error_t begin(const keymaster1_device_t* api, keymaster_purpose_t purpose,
                               const keymaster_key_blob_t* blob, const keymaster_key_param_set_t* params,
                               keymaster_key_param_set_t* output, keymaster_operation_handle_t* handle) {
    if (output) *output = {};
    if (!handle) return KM_ERROR_OUTPUT_PARAMETER_NULL;
    *handle = 0;
    if (purpose != KM_PURPOSE_SIGN && purpose != KM_PURPOSE_VERIFY &&
        purpose != KM_PURPOSE_ENCRYPT && purpose != KM_PURPOSE_DECRYPT) return KM_ERROR_UNSUPPORTED_PURPOSE;
    if (validate_blob(blob) != KM_ERROR_OK) return KM_ERROR_INVALID_KEY_BLOB;
    if (!authorized(blob, KM_TAG_PURPOSE, purpose)) return KM_ERROR_INCOMPATIBLE_PURPOSE;
    const auto* digest_param = parameter(params, KM_TAG_DIGEST);
    const auto* padding_param = parameter(params, KM_TAG_PADDING);
    uint32_t digest = digest_param ? digest_param->enumerated : static_cast<uint32_t>(KM_DIGEST_NONE);
    if (digest != KM_DIGEST_NONE && !rsa_digest(digest)) return KM_ERROR_UNSUPPORTED_DIGEST;
    if (!authorized(blob, KM_TAG_DIGEST, digest)) return KM_ERROR_INCOMPATIBLE_DIGEST;
    if (!padding_param) return KM_ERROR_UNSUPPORTED_PADDING_MODE;
    auto padding = static_cast<keymaster_padding_t>(padding_param->enumerated);
    if (!authorized(blob, KM_TAG_PADDING, padding)) return KM_ERROR_INCOMPATIBLE_PADDING_MODE;
    bool signing = purpose == KM_PURPOSE_SIGN || purpose == KM_PURPOSE_VERIFY;
    if (padding != KM_PAD_NONE &&
        !(signing && (padding == KM_PAD_RSA_PKCS1_1_5_SIGN || padding == KM_PAD_RSA_PSS)) &&
        !(!signing && (padding == KM_PAD_RSA_PKCS1_1_5_ENCRYPT || padding == KM_PAD_RSA_OAEP)))
        return KM_ERROR_INCOMPATIBLE_PADDING_MODE;
    if ((padding == KM_PAD_RSA_PSS || padding == KM_PAD_RSA_OAEP) && digest == KM_DIGEST_NONE)
        return KM_ERROR_INCOMPATIBLE_DIGEST;
    if ((padding == KM_PAD_NONE || padding == KM_PAD_RSA_PKCS1_1_5_ENCRYPT) && digest != KM_DIGEST_NONE)
        return KM_ERROR_INCOMPATIBLE_DIGEST;
    const auto* client_id = parameter(params, KM_TAG_APPLICATION_ID);
    const auto* app_data = parameter(params, KM_TAG_APPLICATION_DATA);
    keymaster_blob_t encoded = {};
    auto error = export_key(api, KM_KEY_FORMAT_X509, blob, client_id ? &client_id->blob : nullptr,
                            app_data ? &app_data->blob : nullptr, &encoded);
    if (error != KM_ERROR_OK) return error;
    const uint8_t* cursor = encoded.data;
    Unique_EVP_PKEY public_key(d2i_PUBKEY(nullptr, &cursor, encoded.data_length), EVP_PKEY_free);
    free(const_cast<uint8_t*>(encoded.data));
    if (!public_key) return KM_ERROR_INVALID_KEY_BLOB;
    Lock lock;
    Device* dev = device(api);
    Operation* available = nullptr;
    for (auto& operation : dev->operations) if (!operation.handle) { available = &operation; break; }
    if (!available) return KM_ERROR_TOO_MANY_OPERATIONS;
    uint64_t random_handle = 0;
    for (size_t attempt = 0; attempt < 32; ++attempt) {
        if (RAND_bytes(reinterpret_cast<uint8_t*>(&random_handle), sizeof(random_handle)) != 1)
            return KM_ERROR_UNKNOWN_ERROR;
        if (random_handle && !operation_for(dev, random_handle)) break;
        random_handle = 0;
    }
    if (!random_handle) return KM_ERROR_UNKNOWN_ERROR;
    auto* copy = static_cast<uint8_t*>(malloc(blob->key_material_size));
    if (!copy) return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    memcpy(copy, blob->key_material, blob->key_material_size);
    available->blob = {copy, blob->key_material_size};
    available->purpose = purpose;
    available->padding = padding;
    available->digest = rsa_digest(digest);
    available->primary = dev->primary;
    available->public_key = public_key.release();
    if (signing && available->digest) {
        available->digest_context = EVP_MD_CTX_new();
        if (!available->digest_context || EVP_DigestInit_ex(available->digest_context, available->digest, nullptr) != 1) {
            clear_operation(available); return KM_ERROR_MEMORY_ALLOCATION_FAILED;
        }
    }
    available->handle = random_handle;
    *handle = random_handle;
    return KM_ERROR_OK;
}
static keymaster_error_t update(const keymaster1_device_t* api, keymaster_operation_handle_t handle,
                                const keymaster_key_param_set_t*, const keymaster_blob_t* input,
                                size_t* consumed, keymaster_key_param_set_t* params, keymaster_blob_t* output) {
    if (params) *params = {};
    if (output) *output = {};
    if (consumed) *consumed = 0;
    Lock lock;
    Operation* operation = operation_for(device(api), handle);
    if (!operation) return KM_ERROR_INVALID_OPERATION_HANDLE;
    if (!consumed || !input || (input->data_length && !input->data)) {
        clear_operation(operation); return KM_ERROR_INVALID_ARGUMENT;
    }
    if (operation->digest_context) {
        if (EVP_DigestUpdate(operation->digest_context, input->data, input->data_length) != 1) {
            clear_operation(operation); return KM_ERROR_UNKNOWN_ERROR;
        }
        *consumed = input->data_length; return KM_ERROR_OK;
    }
    size_t modulus_length = read_integer(operation->blob.key_material + 36, 4) / 8;
    if (input->data_length > modulus_length - operation->used) {
        clear_operation(operation); return KM_ERROR_INVALID_INPUT_LENGTH;
    }
    if (input->data_length) memcpy(operation->input + operation->used, input->data, input->data_length);
    operation->used += input->data_length;
    *consumed = input->data_length;
    return KM_ERROR_OK;
}
static keymaster_error_t finish(const keymaster1_device_t* api, keymaster_operation_handle_t handle,
                                const keymaster_key_param_set_t*, const keymaster_blob_t* signature,
                                keymaster_key_param_set_t* params, keymaster_blob_t* output) {
    if (params) *params = {};
    if (output) *output = {};
    Lock lock;
    Operation* operation = operation_for(device(api), handle);
    if (!operation) return KM_ERROR_INVALID_OPERATION_HANDLE;
    struct Cleanup { Operation* operation; ~Cleanup() { clear_operation(operation); } } cleanup{operation};
    if (!output) return KM_ERROR_OUTPUT_PARAMETER_NULL;
    uint8_t hash[EVP_MAX_MD_SIZE];
    unsigned hash_length = 0;
    const uint8_t* input = operation->input;
    size_t input_length = operation->used;
    if (operation->digest_context) {
        if (EVP_DigestFinal_ex(operation->digest_context, hash, &hash_length) != 1) return KM_ERROR_UNKNOWN_ERROR;
        input = hash; input_length = hash_length;
    }
    RSA_METHOD method = {};
    std::unique_ptr<ENGINE, decltype(&ENGINE_free)> engine(ENGINE_new(), ENGINE_free);
    Unique_EVP_PKEY pkey(EVP_PKEY_new(), EVP_PKEY_free);
    method.common.is_static = 1;
    method.flags = RSA_FLAG_OPAQUE;
    method.private_transform = private_transform;
    if (!pkey || !engine || !ENGINE_set_RSA_method(engine.get(), &method, sizeof(method)))
        return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    // The RSA method and ENGINE outlive every RSA/EVP context using their callback.
    Unique_RSA rsa(RSA_new_method(engine.get()), RSA_free);
    const RSA* public_rsa = EVP_PKEY_get0_RSA(operation->public_key);
    if (!rsa || !public_rsa) return KM_ERROR_UNKNOWN_ERROR;
    rsa->n = BN_dup(public_rsa->n); rsa->e = BN_dup(public_rsa->e);
    if (!rsa->n || !rsa->e || !RSA_set_ex_data(rsa.get(), rsa_operation_index, operation))
        return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    if (!EVP_PKEY_assign_RSA(pkey.get(), rsa.get())) return KM_ERROR_UNKNOWN_ERROR;
    (void)rsa.release();
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(EVP_PKEY_CTX_new(pkey.get(), nullptr), EVP_PKEY_CTX_free);
    if (!context) return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    int initialized = 0;
    switch (operation->purpose) {
    case KM_PURPOSE_SIGN: initialized = EVP_PKEY_sign_init(context.get()); break;
    case KM_PURPOSE_VERIFY: initialized = EVP_PKEY_verify_init(context.get()); break;
    case KM_PURPOSE_ENCRYPT: initialized = EVP_PKEY_encrypt_init(context.get()); break;
    case KM_PURPOSE_DECRYPT: initialized = EVP_PKEY_decrypt_init(context.get()); break;
    default: return KM_ERROR_UNSUPPORTED_PURPOSE;
    }
    // RsaKeymaster1WrappedOperation::Begin preserves PKCS#1 v1.5 modes, while
    // Keymaster1Engine::rsa_sign_raw supplies DigestInfo for digest NONE.
    // EVP handles both that raw v1.5 contract and complete digested operations.
    int padding = RSA_NO_PADDING;
    if (operation->padding == KM_PAD_RSA_PSS) padding = RSA_PKCS1_PSS_PADDING;
    else if (operation->padding == KM_PAD_RSA_OAEP) padding = RSA_PKCS1_OAEP_PADDING;
    else if (operation->padding != KM_PAD_NONE) padding = RSA_PKCS1_PADDING;
    if (initialized != 1 || EVP_PKEY_CTX_set_rsa_padding(context.get(), padding) != 1)
        return KM_ERROR_UNKNOWN_ERROR;
    if (operation->digest) {
        if (padding == RSA_PKCS1_OAEP_PADDING) {
            if (EVP_PKEY_CTX_set_rsa_oaep_md(context.get(), operation->digest) != 1 ||
                EVP_PKEY_CTX_set_rsa_mgf1_md(context.get(), EVP_sha1()) != 1) return KM_ERROR_UNKNOWN_ERROR;
        } else if (EVP_PKEY_CTX_set_signature_md(context.get(), operation->digest) != 1) return KM_ERROR_UNKNOWN_ERROR;
    }
    if (padding == RSA_PKCS1_PSS_PADDING &&
        (EVP_PKEY_CTX_set_rsa_pss_saltlen(context.get(), EVP_MD_size(operation->digest)) != 1 ||
         EVP_PKEY_CTX_set_rsa_mgf1_md(context.get(), operation->digest) != 1)) return KM_ERROR_UNKNOWN_ERROR;
    uint8_t padded[MAX_RSA_BYTES] = {};
    size_t length = RSA_size(EVP_PKEY_get0_RSA(pkey.get()));
    if (padding == RSA_NO_PADDING || operation->purpose == KM_PURPOSE_DECRYPT) {
        if (input_length > length) return KM_ERROR_INVALID_INPUT_LENGTH;
        memcpy(padded + length - input_length, input, input_length);
        input = padded; input_length = length;
    }
    if (operation->purpose == KM_PURPOSE_VERIFY) {
        if (!signature || !signature->data) return KM_ERROR_INVALID_ARGUMENT;
        return EVP_PKEY_verify(context.get(), signature->data, signature->data_length, input, input_length) == 1
            ? KM_ERROR_OK : KM_ERROR_VERIFICATION_FAILED;
    }
    auto* bytes = static_cast<uint8_t*>(malloc(length));
    if (!bytes) return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    int result = 0;
    if (operation->purpose == KM_PURPOSE_SIGN)
        result = EVP_PKEY_sign(context.get(), bytes, &length, input, input_length);
    else if (operation->purpose == KM_PURPOSE_ENCRYPT)
        result = EVP_PKEY_encrypt(context.get(), bytes, &length, input, input_length);
    else result = EVP_PKEY_decrypt(context.get(), bytes, &length, input, input_length);
    if (result != 1) { free(bytes); return KM_ERROR_UNKNOWN_ERROR; }
    *output = {bytes, length};
    return KM_ERROR_OK;
}
static keymaster_error_t abort_operation(const keymaster1_device_t* api, keymaster_operation_handle_t handle) {
    Lock lock;
    Operation* operation = operation_for(device(api), handle);
    if (!operation) return KM_ERROR_INVALID_OPERATION_HANDLE;
    clear_operation(operation);
    return KM_ERROR_OK;
}
static int close_device(hw_device_t* hardware) {
    if (!hardware) return -EINVAL;
    Lock lock;
    auto* dev = reinterpret_cast<Device*>(hardware);
    for (auto& operation : dev->operations) clear_operation(&operation);
    CK_RV closed = C_CloseSession(dev->primary);
    CK_RV finalized = C_Finalize(nullptr);
    free(dev);
    module_open = false;
    return closed == CKR_OK && finalized == CKR_OK ? 0 : -EIO;
}
static int open_device(const hw_module_t* module, const char* name, hw_device_t** output) {
    if (!output) return -EINVAL;
    *output = nullptr;
    if (!module || !name || strcmp(name, KEYSTORE_KEYMASTER) != 0) return -EINVAL;
    Lock lock;
    if (module_open) return -EBUSY;
    auto* dev = static_cast<Device*>(calloc(1, sizeof(Device)));
    if (!dev) return -ENOMEM;
    if (C_Initialize(nullptr) != CKR_OK) { free(dev); return -ENODEV; }
    dev->primary = CK_INVALID_HANDLE;
    if (C_OpenSession(CKV_TOKEN_USER, CKF_SERIAL_SESSION | CKF_RW_SESSION, nullptr, nullptr,
                       &dev->primary) != CKR_OK || dev->primary == CK_INVALID_HANDLE) {
        C_Finalize(nullptr); free(dev); return -ENODEV;
    }
    auto& api = dev->api;
    api.common.tag = HARDWARE_DEVICE_TAG;
    api.common.version = 1;
    api.common.module = const_cast<hw_module_t*>(module);
    api.common.close = close_device;
    api.flags = 0;
    api.context = dev;
    api.get_supported_algorithms = algorithms;
    api.get_supported_block_modes = block_modes;
    api.get_supported_padding_modes = paddings;
    api.get_supported_digests = digests;
    api.get_supported_import_formats = import_formats;
    api.get_supported_export_formats = export_formats;
    api.add_rng_entropy = entropy;
    api.generate_key = generate;
    api.get_key_characteristics = get_characteristics;
    api.import_key = import_key;
    api.export_key = export_key;
    api.delete_key = delete_key;
    api.delete_all_keys = delete_all;
    api.begin = begin;
    api.update = update;
    api.finish = finish;
    api.abort = abort_operation;
    if (rsa_operation_index < 0) rsa_operation_index = RSA_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    if (rsa_operation_index < 0) { C_CloseSession(dev->primary); C_Finalize(nullptr); free(dev); return -ENOMEM; }
    module_open = true;
    *output = &api.common;
    return 0;
}
// Keymaster1PassthroughContext::GetKeyFactory sends every algorithm to this
// device, and requiresSoftwareDigesting passes AES through unwrapped, so AES,
// HMAC and EC keys live in an embedded pure software SoftKeymasterDevice.
struct Composite {
    keymaster1_device_t api = {};
    keymaster1_device_t* tee = nullptr;
    std::unique_ptr<keymaster::SoftKeymasterDevice> software;
    std::mutex mutex;
    struct Slot { uint64_t handle = 0; keymaster1_device_t* owner = nullptr; uint64_t inner = 0; };
    Slot operations[MAX_OPERATIONS];
    uint64_t next_handle = 0;
};
static Composite* composite(const keymaster1_device_t* api) { return static_cast<Composite*>(api->context); }
static keymaster1_device_t* software(const keymaster1_device_t* api) { return composite(api)->software->keymaster_device(); }
static keymaster_error_t unwrap(const keymaster1_device_t* api, const keymaster_key_blob_t* blob,
                                keymaster1_device_t** owner, keymaster_key_blob_t* inner) {
    if (!blob || !blob->key_material || blob->key_material_size <= 4) return KM_ERROR_INVALID_KEY_BLOB;
    if (memcmp(blob->key_material, "TTEE", 4) == 0) *owner = composite(api)->tee;
    else if (memcmp(blob->key_material, "TSFT", 4) == 0) *owner = software(api);
    else return KM_ERROR_INVALID_KEY_BLOB;
    *inner = {blob->key_material + 4, blob->key_material_size - 4};
    return KM_ERROR_OK;
}
static keymaster_error_t wrap(keymaster1_device_t* owner, Composite* dev, keymaster_key_blob_t* blob) {
    if (!blob || !blob->key_material || blob->key_material_size > SIZE_MAX - 4) return KM_ERROR_INVALID_KEY_BLOB;
    auto* bytes = static_cast<uint8_t*>(malloc(blob->key_material_size + 4));
    if (!bytes) return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    memcpy(bytes, owner == dev->tee ? "TTEE" : "TSFT", 4);
    memcpy(bytes + 4, blob->key_material, blob->key_material_size);
    free(const_cast<uint8_t*>(blob->key_material));
    blob->key_material = bytes; blob->key_material_size += 4;
    return KM_ERROR_OK;
}
static keymaster_error_t composite_create(const keymaster1_device_t* api,
    const keymaster_key_param_set_t* params, keymaster_key_format_t format,
    const keymaster_blob_t* input, keymaster_key_blob_t* blob,
    keymaster_key_characteristics_t** output) {
    if (!blob) return KM_ERROR_OUTPUT_PARAMETER_NULL;
    *blob = {}; if (output) *output = nullptr;
    auto* dev = composite(api);
    std::lock_guard<std::mutex> lock(dev->mutex);
    const auto* algorithm = parameter(params, KM_TAG_ALGORITHM);
    if (!algorithm) return KM_ERROR_INVALID_ARGUMENT;
    auto* owner = algorithm->enumerated == KM_ALGORITHM_RSA ? dev->tee : software(api);
    auto error = input ? owner->import_key(owner, params, format, input, blob, output)
                       : owner->generate_key(owner, params, blob, output);
    if (error == KM_ERROR_OK) error = wrap(owner, dev, blob);
    if (error != KM_ERROR_OK) {
        if (blob->key_material) { owner->delete_key(owner, blob); free(const_cast<uint8_t*>(blob->key_material)); *blob = {}; }
        if (output && *output) { keymaster_free_characteristics(*output); free(*output); *output = nullptr; }
    }
    return error;
}
static keymaster_error_t composite_generate(const keymaster1_device_t* api,
    const keymaster_key_param_set_t* params, keymaster_key_blob_t* blob, keymaster_key_characteristics_t** output) {
    return composite_create(api, params, KM_KEY_FORMAT_RAW, nullptr, blob, output);
}
static keymaster_error_t composite_import(const keymaster1_device_t* api,
    const keymaster_key_param_set_t* params, keymaster_key_format_t format, const keymaster_blob_t* input,
    keymaster_key_blob_t* blob, keymaster_key_characteristics_t** output) {
    if (!input) return KM_ERROR_INVALID_ARGUMENT;
    return composite_create(api, params, format, input, blob, output);
}
static keymaster_error_t composite_characteristics(const keymaster1_device_t* api,
    const keymaster_key_blob_t* blob, const keymaster_blob_t* client, const keymaster_blob_t* app,
    keymaster_key_characteristics_t** output) {
    if (output) *output = nullptr;
    std::lock_guard<std::mutex> lock(composite(api)->mutex);
    keymaster1_device_t* owner; keymaster_key_blob_t inner;
    auto error = unwrap(api, blob, &owner, &inner);
    return error == KM_ERROR_OK ? owner->get_key_characteristics(owner, &inner, client, app, output) : error;
}
static keymaster_error_t composite_export(const keymaster1_device_t* api, keymaster_key_format_t format,
    const keymaster_key_blob_t* blob, const keymaster_blob_t* client, const keymaster_blob_t* app, keymaster_blob_t* output) {
    if (output) *output = {};
    std::lock_guard<std::mutex> lock(composite(api)->mutex);
    keymaster1_device_t* owner; keymaster_key_blob_t inner;
    auto error = unwrap(api, blob, &owner, &inner);
    return error == KM_ERROR_OK ? owner->export_key(owner, format, &inner, client, app, output) : error;
}
static keymaster_error_t composite_delete(const keymaster1_device_t* api, const keymaster_key_blob_t* blob) {
    std::lock_guard<std::mutex> lock(composite(api)->mutex);
    keymaster1_device_t* owner; keymaster_key_blob_t inner;
    auto error = unwrap(api, blob, &owner, &inner);
    return error == KM_ERROR_OK ? owner->delete_key(owner, &inner) : error;
}
static keymaster_error_t composite_begin(const keymaster1_device_t* api, keymaster_purpose_t purpose,
    const keymaster_key_blob_t* blob, const keymaster_key_param_set_t* params,
    keymaster_key_param_set_t* output, keymaster_operation_handle_t* handle) {
    if (output) *output = {};
    if (!handle) return KM_ERROR_OUTPUT_PARAMETER_NULL;
    *handle = 0;
    auto* dev = composite(api);
    std::lock_guard<std::mutex> lock(dev->mutex);
    keymaster1_device_t* owner; keymaster_key_blob_t inner;
    auto error = unwrap(api, blob, &owner, &inner);
    if (error != KM_ERROR_OK) return error;
    Composite::Slot* available = nullptr;
    for (auto& slot : dev->operations) if (!slot.handle) { available = &slot; break; }
    if (!available || dev->next_handle == UINT64_MAX) return KM_ERROR_TOO_MANY_OPERATIONS;
    uint64_t inner_handle = 0;
    error = owner->begin(owner, purpose, &inner, params, output, &inner_handle);
    if (error != KM_ERROR_OK) return error;
    available->handle = ++dev->next_handle; available->inner = inner_handle; available->owner = owner;
    *handle = available->handle;
    return KM_ERROR_OK;
}
static keymaster_error_t composite_update(const keymaster1_device_t* api, uint64_t handle,
    const keymaster_key_param_set_t* params, const keymaster_blob_t* input, size_t* consumed,
    keymaster_key_param_set_t* out_params, keymaster_blob_t* output) {
    if (consumed) *consumed = 0;
    if (out_params) *out_params = {};
    if (output) *output = {};
    auto* dev = composite(api);
    std::lock_guard<std::mutex> lock(dev->mutex);
    for (auto& slot : dev->operations) if (handle && slot.handle == handle) {
        auto error = slot.owner->update(slot.owner, slot.inner, params, input, consumed, out_params, output);
        if (error != KM_ERROR_OK) { slot.owner->abort(slot.owner, slot.inner); slot = {}; }
        return error;
    }
    return KM_ERROR_INVALID_OPERATION_HANDLE;
}
static keymaster_error_t composite_finish(const keymaster1_device_t* api, uint64_t handle,
    const keymaster_key_param_set_t* params, const keymaster_blob_t* signature,
    keymaster_key_param_set_t* out_params, keymaster_blob_t* output) {
    if (out_params) *out_params = {};
    if (output) *output = {};
    auto* dev = composite(api);
    std::lock_guard<std::mutex> lock(dev->mutex);
    for (auto& slot : dev->operations) if (handle && slot.handle == handle) {
        auto error = slot.owner->finish(slot.owner, slot.inner, params, signature, out_params, output);
        slot = {}; return error;
    }
    return KM_ERROR_INVALID_OPERATION_HANDLE;
}
static keymaster_error_t composite_abort(const keymaster1_device_t* api, uint64_t handle) {
    auto* dev = composite(api);
    std::lock_guard<std::mutex> lock(dev->mutex);
    for (auto& slot : dev->operations) if (handle && slot.handle == handle) {
        auto error = slot.owner->abort(slot.owner, slot.inner); slot = {}; return error;
    }
    return KM_ERROR_INVALID_OPERATION_HANDLE;
}
static int composite_close(hw_device_t* hardware) {
    if (!hardware) return -EINVAL;
    auto* dev = composite(reinterpret_cast<keymaster1_device_t*>(hardware));
    {
        std::lock_guard<std::mutex> lock(dev->mutex);
        for (auto& slot : dev->operations) if (slot.handle) slot.owner->abort(slot.owner, slot.inner);
        dev->software.reset();
    }
    int result = dev->tee->common.close(&dev->tee->common);
    delete dev;
    return result;
}
static int composite_open(const hw_module_t* module, const char* name, hw_device_t** output) {
    if (!output) return -EINVAL;
    *output = nullptr;
    std::unique_ptr<Composite> dev(new (std::nothrow) Composite);
    if (!dev) return -ENOMEM;
#if TUNA_SOFT_KEYMASTER_HAS_VERSION
    dev->software.reset(new (std::nothrow) keymaster::SoftKeymasterDevice(keymaster::KmVersion::KEYMASTER_3));
#else
    dev->software.reset(new (std::nothrow) keymaster::SoftKeymasterDevice);
#endif
    if (!dev->software) return -ENOMEM;
    hw_device_t* raw = nullptr;
    int result = open_device(module, name, &raw);
    if (result) return result;
    dev->tee = reinterpret_cast<keymaster1_device_t*>(raw);
    auto& api = dev->api;
    api = *dev->tee;
    api.context = dev.get(); api.common.close = composite_close; api.flags = 0;
    api.generate_key = composite_generate; api.import_key = composite_import;
    api.get_key_characteristics = composite_characteristics; api.export_key = composite_export;
    api.delete_key = composite_delete;
    api.delete_all_keys = [](const keymaster1_device_t* outer) {
        std::lock_guard<std::mutex> lock(composite(outer)->mutex);
        auto* inner = software(outer); return inner->delete_all_keys(inner);
    };
    api.add_rng_entropy = [](const keymaster1_device_t* outer, const uint8_t* data, size_t length) {
        std::lock_guard<std::mutex> lock(composite(outer)->mutex);
        auto* inner = software(outer);
        auto sw_error = inner->add_rng_entropy(inner, data, length);
        auto* tee = composite(outer)->tee;
        auto tee_error = tee->add_rng_entropy(tee, data, length);
        return sw_error != KM_ERROR_OK ? sw_error : tee_error;
    };
    api.get_supported_algorithms = [](const keymaster1_device_t* outer, keymaster_algorithm_t** values, size_t* count) {
        auto* inner = software(outer); return inner->get_supported_algorithms(inner, values, count);
    };
#define TUNA_CAPABILITY(callback, Type) \
    api.callback = [](const keymaster1_device_t* outer, keymaster_algorithm_t algorithm, \
                      keymaster_purpose_t purpose, Type** values, size_t* count) { \
        auto* inner = software(outer); return inner->callback(inner, algorithm, purpose, values, count); \
    }
    TUNA_CAPABILITY(get_supported_block_modes, keymaster_block_mode_t);
    TUNA_CAPABILITY(get_supported_padding_modes, keymaster_padding_t);
    TUNA_CAPABILITY(get_supported_digests, keymaster_digest_t);
#undef TUNA_CAPABILITY
#define TUNA_FORMAT(callback) \
    api.callback = [](const keymaster1_device_t* outer, keymaster_algorithm_t algorithm, \
                      keymaster_key_format_t** values, size_t* count) { \
        auto* inner = software(outer); return inner->callback(inner, algorithm, values, count); \
    }
    TUNA_FORMAT(get_supported_import_formats);
    TUNA_FORMAT(get_supported_export_formats);
#undef TUNA_FORMAT
    api.begin = composite_begin; api.update = composite_update;
    api.finish = composite_finish; api.abort = composite_abort;
    *output = &api.common;
    (void)dev.release();
    return 0;
}
static hw_module_methods_t module_methods = {composite_open};

}  // namespace

keystore_module_t HAL_MODULE_INFO_SYM __attribute__((visibility("default"))) = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = KEYMASTER_MODULE_API_VERSION_1_0,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = KEYSTORE_HARDWARE_MODULE_ID,
        .name = "OMAP4 TF RSA and software keymaster",
        .author = "The Android Open Source Project",
        .methods = &module_methods,
        .dso = nullptr,
        .reserved = {},
    },
};
