#include <aidl/android/hardware/security/keymint/ErrorCode.h>
#include <aidl/android/system/keystore2/IKeystoreService.h>
#include <aidl/android/system/keystore2/IKeystoreSecurityLevel.h>
#include <aidl/android/system/keystore2/IKeystoreOperation.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <keymint_support/authorization_set.h>
#include <stdio.h>
#include <memory>

namespace {
namespace keymint = aidl::android::hardware::security::keymint;
namespace ks2 = aidl::android::system::keystore2;
static int error_code(const ndk::ScopedAStatus& status) {
    return status.isOk() ? 0 : status.getExceptionCode() == EX_SERVICE_SPECIFIC ?
        status.getServiceSpecificError() : status.getExceptionCode();
}
static ks2::KeyDescriptor descriptor(const char* alias) {
    ks2::KeyDescriptor key;
    key.domain = ks2::Domain::APP; key.nspace = -1; key.alias = alias;
    return key;
}
static bool probe(const std::shared_ptr<ks2::IKeystoreService>& service,
                  const std::shared_ptr<ks2::IKeystoreSecurityLevel>& security, bool auth_bound) {
    const char* name = auth_bound ? "auth-bound-begin-refused" : "control-begin-accepted";
    auto key = descriptor(auth_bound ? "tuna_auth_probe" : "tuna_auth_probe_control");
    keymint::AuthorizationSetBuilder params;
    params.AesEncryptionKey(256).GcmModeMinMacLen(128);
    if (auth_bound) {
        params.Authorization(keymint::TAG_USER_SECURE_ID, uint64_t{1})
            .Authorization(keymint::TAG_USER_AUTH_TYPE, keymint::HardwareAuthenticatorType::PASSWORD)
            .Authorization(keymint::TAG_AUTH_TIMEOUT, uint32_t{15});
    } else params.Authorization(keymint::TAG_NO_AUTH_REQUIRED);
    ks2::KeyMetadata metadata;
    auto status = security->generateKey(key, {}, params.vector_data(), 0, {}, &metadata);
    if (!status.isOk()) {
        // keystore2 answers UNINITIALIZED (3) for an auth-bound key while the user has no
        // lock-screen credential, because no super key exists to encrypt it.
        printf("FAIL %s generate %d%s\n", name, error_code(status),
               error_code(status) == 3 ? " (set a lock-screen credential first)" : "");
        return false;
    }
    keymint::AuthorizationSetBuilder operation_params;
    operation_params.Authorization(keymint::TAG_PURPOSE, keymint::KeyPurpose::ENCRYPT).GcmModeMacLen(128);
    ks2::CreateOperationResponse operation;
    status = security->createOperation(metadata.key, operation_params.vector_data(), false, &operation);
    int code = error_code(status);
    bool success = auth_bound ? status.getExceptionCode() == EX_SERVICE_SPECIFIC &&
        code == static_cast<int>(keymint::ErrorCode::KEY_USER_NOT_AUTHENTICATED) : status.isOk() && operation.iOperation;
    if (success) printf("PASS %s\n", name);
    else printf("FAIL %s %d\n", name, code);
    if (operation.iOperation) {
        auto aborted = operation.iOperation->abort();
        if (!aborted.isOk()) { printf("FAIL operation-abort %d\n", error_code(aborted)); success = false; }
    }
    auto deleted = service->deleteKey(key);
    if (!deleted.isOk()) { printf("FAIL key-delete %d\n", error_code(deleted)); success = false; }
    return success;
}
} // namespace

int main() {
    ABinderProcess_startThreadPool();
    ndk::SpAIBinder binder(AServiceManager_getService("android.system.keystore2.IKeystoreService/default"));
    auto service = ks2::IKeystoreService::fromBinder(binder);
    if (!service) { printf("FAIL service-connect\n"); return 1; }
    std::shared_ptr<ks2::IKeystoreSecurityLevel> security;
    auto status = service->getSecurityLevel(keymint::SecurityLevel::TRUSTED_ENVIRONMENT, &security);
    if (!status.isOk() || !security) { printf("FAIL security-level %d\n", error_code(status)); return 1; }
    bool refused = probe(service, security, true);
    bool accepted = probe(service, security, false);
    return refused && accepted ? 0 : 1;
}
