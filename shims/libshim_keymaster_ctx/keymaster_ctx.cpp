/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <memory>
#include <new>

#include <keymaster/contexts/keymaster1_passthrough_context.h>
#include <keymaster/contexts/keymaster2_passthrough_context.h>
#include <keymaster/keymaster_configuration.h>

namespace {

using namespace keymaster;

// The Android 9 libskeymaster3device allocates the passthrough context itself with a fixed size (104 and 72
// bytes on arm64, 56 and 40 on arm: operator new in CreateSKeymasterDevice(keymaster1_device*) and
// (keymaster2_device*)) and runs a one-argument constructor that system/keymaster has replaced by a
// (KmVersion, device) one. The current classes are larger than that storage, so the storage holds a context
// that only forwards to the real one, which lives on the heap.
class ForwardingContext : public KeymasterContext {
  public:
    explicit ForwardingContext(KeymasterContext* context) : context_(context) {
        context_->SetSystemVersion(GetOsVersion(), GetOsPatchlevel());
    }

    KmVersion GetKmVersion() const override { return context_->GetKmVersion(); }
    keymaster_error_t SetSystemVersion(uint32_t os_version, uint32_t os_patchlevel) override {
        return context_->SetSystemVersion(os_version, os_patchlevel);
    }
    void GetSystemVersion(uint32_t* os_version, uint32_t* os_patchlevel) const override {
        context_->GetSystemVersion(os_version, os_patchlevel);
    }
    const KeyFactory* GetKeyFactory(keymaster_algorithm_t algorithm) const override {
        return context_->GetKeyFactory(algorithm);
    }
    const OperationFactory* GetOperationFactory(keymaster_algorithm_t algorithm,
                                                keymaster_purpose_t purpose) const override {
        return context_->GetOperationFactory(algorithm, purpose);
    }
    const keymaster_algorithm_t* GetSupportedAlgorithms(size_t* algorithms_count) const override {
        return context_->GetSupportedAlgorithms(algorithms_count);
    }
    keymaster_error_t UpgradeKeyBlob(const KeymasterKeyBlob& key_to_upgrade,
                                     const AuthorizationSet& upgrade_params,
                                     KeymasterKeyBlob* upgraded_key) const override {
        return context_->UpgradeKeyBlob(key_to_upgrade, upgrade_params, upgraded_key);
    }
    keymaster_error_t ParseKeyBlob(const KeymasterKeyBlob& blob,
                                   const AuthorizationSet& additional_params,
                                   UniquePtr<Key>* key) const override {
        return context_->ParseKeyBlob(blob, additional_params, key);
    }
    keymaster_error_t DeleteKey(const KeymasterKeyBlob& blob) const override {
        return context_->DeleteKey(blob);
    }
    keymaster_error_t DeleteAllKeys() const override { return context_->DeleteAllKeys(); }
    keymaster_error_t AddRngEntropy(const uint8_t* buf, size_t length) const override {
        return context_->AddRngEntropy(buf, length);
    }
    KeymasterEnforcement* enforcement_policy() override { return context_->enforcement_policy(); }
    AttestationContext* attestation_context() override { return context_->attestation_context(); }
    CertificateChain GenerateAttestation(const Key& key, const AuthorizationSet& attest_params,
                                         UniquePtr<Key> attest_key,
                                         const KeymasterBlob& issuer_subject,
                                         keymaster_error_t* error) const override {
        return context_->GenerateAttestation(key, attest_params, std::move(attest_key),
                                             issuer_subject, error);
    }
    CertificateChain GenerateSelfSignedCertificate(const Key& key,
                                                   const AuthorizationSet& cert_params,
                                                   bool fake_signature,
                                                   keymaster_error_t* error) const override {
        return context_->GenerateSelfSignedCertificate(key, cert_params, fake_signature, error);
    }
    keymaster_error_t UnwrapKey(const KeymasterKeyBlob& wrapped_key_blob,
                                const KeymasterKeyBlob& wrapping_key_blob,
                                const AuthorizationSet& wrapping_key_params,
                                const KeymasterKeyBlob& masking_key,
                                AuthorizationSet* wrapped_key_params,
                                keymaster_key_format_t* wrapped_key_format,
                                KeymasterKeyBlob* wrapped_key_material) const override {
        return context_->UnwrapKey(wrapped_key_blob, wrapping_key_blob, wrapping_key_params,
                                   masking_key, wrapped_key_params, wrapped_key_format,
                                   wrapped_key_material);
    }
    SecureKeyStorage* secure_key_storage() override { return context_->secure_key_storage(); }
    SecureDeletionSecretStorage* secure_deletion_secret_storage() override {
        return context_->secure_deletion_secret_storage();
    }
    keymaster_error_t CheckConfirmationToken(
            const uint8_t* input_data, size_t input_data_size,
            const uint8_t confirmation_token[kConfirmationTokenSize]) const override {
        return context_->CheckConfirmationToken(input_data, input_data_size, confirmation_token);
    }
    RemoteProvisioningContext* GetRemoteProvisioningContext() const override {
        return context_->GetRemoteProvisioningContext();
    }
    keymaster_error_t SetVerifiedBootInfo(std::string_view verified_boot_state,
                                          std::string_view bootloader_state,
                                          const std::vector<uint8_t>& vbmeta_digest) override {
        return context_->SetVerifiedBootInfo(verified_boot_state, bootloader_state, vbmeta_digest);
    }
    keymaster_error_t SetVendorPatchlevel(uint32_t vendor_patchlevel) override {
        return context_->SetVendorPatchlevel(vendor_patchlevel);
    }
    keymaster_error_t SetBootPatchlevel(uint32_t boot_patchlevel) override {
        return context_->SetBootPatchlevel(boot_patchlevel);
    }
    std::optional<uint32_t> GetVendorPatchlevel() const override {
        return context_->GetVendorPatchlevel();
    }
    std::optional<uint32_t> GetBootPatchlevel() const override {
        return context_->GetBootPatchlevel();
    }
    keymaster_error_t SetAttestationIds(const SetAttestationIdsRequest& request) override {
        return context_->SetAttestationIds(request);
    }
    keymaster_error_t SetAttestationIdsKM3(const SetAttestationIdsKM3Request& request) override {
        return context_->SetAttestationIdsKM3(request);
    }
    keymaster_error_t SetModuleHash(const keymaster_blob_t& module_hash) override {
        return context_->SetModuleHash(module_hash);
    }

  private:
    std::unique_ptr<KeymasterContext> context_;
};

#ifdef __LP64__
constexpr size_t kKm1ContextAllocSize = 104;
constexpr size_t kKm2ContextAllocSize = 72;
#else
constexpr size_t kKm1ContextAllocSize = 56;
constexpr size_t kKm2ContextAllocSize = 40;
#endif
static_assert(sizeof(ForwardingContext) <= kKm1ContextAllocSize &&
                      sizeof(ForwardingContext) <= kKm2ContextAllocSize,
              "ForwardingContext no longer fits the blob's allocation");

}  // namespace

#define SHIM_EXPORT extern "C" __attribute__((visibility("default"), used))

SHIM_EXPORT void* shim_Keymaster1PassthroughContext(void* self, keymaster1_device_t* dev)
        __asm__("_ZN9keymaster28Keymaster1PassthroughContextC1EP17keymaster1_device");
SHIM_EXPORT void* shim_Keymaster2PassthroughContext(void* self, keymaster2_device_t* dev)
        __asm__("_ZN9keymaster28Keymaster2PassthroughContextC1EP17keymaster2_device");

void* shim_Keymaster1PassthroughContext(void* self, keymaster1_device_t* dev) {
    new (self) ForwardingContext(new Keymaster1PassthroughContext(KmVersion::KEYMASTER_3, dev));
    return self;
}

void* shim_Keymaster2PassthroughContext(void* self, keymaster2_device_t* dev) {
    new (self) ForwardingContext(new Keymaster2PassthroughContext(KmVersion::KEYMASTER_3, dev));
    return self;
}
