#include "schemaregistry/rules/encryption/EncryptionExecutor.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <random>

#include "absl/strings/escaping.h"
#include "absl/strings/str_format.h"
#include "schemaregistry/rest/RestException.h"
#include "schemaregistry/rules/encryption/EncryptionRegistry.h"
#include "schemaregistry/serdes/RuleRegistry.h"
#include "schemaregistry/serdes/avro/AvroTypes.h"
#include "schemaregistry/serdes/json/JsonTypes.h"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"
#include "tink/aead.h"
#include "tink/aead_config.h"
#include "tink/aead_key_templates.h"
#include "tink/deterministic_aead.h"
#include "tink/deterministic_aead_config.h"
#include "tink/deterministic_aead_key_templates.h"
#include "tink/keyset_handle.h"
#include "tink/registry.h"
#include "tink/util/status.h"
#include "tink/util/statusor.h"
#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#define htobe32(x) OSSwapHostToBigInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#elif defined(_WIN32)
#include <winsock2.h>
#define htobe32(x) htonl(x)
#define be32toh(x) ntohl(x)
#else
#include <endian.h>
#endif

namespace schemaregistry::rules::encryption {

using namespace schemaregistry::serdes;
using namespace schemaregistry::rest;

// SystemClock implementation
int64_t SystemClock::now() const {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration)
        .count();
}

// FakeClock implementation
FakeClock::FakeClock(int64_t time) : time_(time) {}

int64_t FakeClock::now() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return time_;
}

void FakeClock::setTime(int64_t time) {
    std::lock_guard<std::mutex> lock(mutex_);
    time_ = time;
}

// Cryptor implementation
Cryptor::Cryptor(schemaregistry::rest::model::Algorithm dek_format)
    : dek_format_(dek_format) {
    // Initialize Tink
    auto status = crypto::tink::AeadConfig::Register();
    if (!status.ok()) {
        throw SerdeError("Failed to register AEAD config: " +
                         std::string(status.message()));
    }

    status = crypto::tink::DeterministicAeadConfig::Register();
    if (!status.ok()) {
        throw SerdeError("Failed to register Deterministic AEAD config: " +
                         std::string(status.message()));
    }

    // Create key template based on algorithm
    switch (dek_format) {
        case schemaregistry::rest::model::Algorithm::Aes128Gcm: {
            key_template_ = crypto::tink::AeadKeyTemplates::Aes128Gcm();
            break;
        }
        case schemaregistry::rest::model::Algorithm::Aes256Gcm: {
            key_template_ = crypto::tink::AeadKeyTemplates::Aes256Gcm();
            break;
        }
        case schemaregistry::rest::model::Algorithm::Aes256Siv: {
            key_template_ =
                crypto::tink::DeterministicAeadKeyTemplates::Aes256Siv();
            break;
        }
        default:
            throw SerdeError("Unsupported DEK algorithm");
    }
}

bool Cryptor::isDeterministic() const {
    return dek_format_ == schemaregistry::rest::model::Algorithm::Aes256Siv;
}

std::vector<uint8_t> Cryptor::generateKey() const {
    auto key_data_result = crypto::tink::Registry::NewKeyData(key_template_);
    if (!key_data_result.ok()) {
        throw SerdeError("Failed to generate key data: " +
                         std::string(key_data_result.status().message()));
    }

    const std::string &key_value = key_data_result.value()->value();
    return std::vector<uint8_t>(key_value.begin(), key_value.end());
}

std::vector<uint8_t> Cryptor::encrypt(
    const std::vector<uint8_t> &dek, const std::vector<uint8_t> &plaintext,
    const std::vector<uint8_t> &associated_data) const {
    google::crypto::tink::KeyData key_data;
    key_data.set_type_url(key_template_.type_url());
    key_data.set_key_material_type(
        google::crypto::tink::KeyData_KeyMaterialType_SYMMETRIC);
    key_data.set_value(std::string(dek.begin(), dek.end()));

    if (isDeterministic()) {
        auto primitive_result = crypto::tink::Registry::GetPrimitive<
            crypto::tink::DeterministicAead>(key_data);
        if (!primitive_result.ok()) {
            throw SerdeError("could not get deterministic aead primitive: " +
                             std::string(primitive_result.status().message()));
        }

        auto primitive = std::move(primitive_result.value());
        auto ciphertext_result = primitive->EncryptDeterministically(
            absl::string_view(reinterpret_cast<const char *>(plaintext.data()),
                              plaintext.size()),
            absl::string_view(
                reinterpret_cast<const char *>(associated_data.data()),
                associated_data.size()));

        if (!ciphertext_result.ok()) {
            throw SerdeError("encryption failed: " +
                             std::string(ciphertext_result.status().message()));
        }

        const std::string &result = ciphertext_result.value();
        return std::vector<uint8_t>(result.begin(), result.end());
    } else {
        auto primitive_result =
            crypto::tink::Registry::GetPrimitive<crypto::tink::Aead>(key_data);
        if (!primitive_result.ok()) {
            throw SerdeError("could not get aead primitive: " +
                             std::string(primitive_result.status().message()));
        }

        auto primitive = std::move(primitive_result.value());
        auto ciphertext_result = primitive->Encrypt(
            absl::string_view(reinterpret_cast<const char *>(plaintext.data()),
                              plaintext.size()),
            absl::string_view(
                reinterpret_cast<const char *>(associated_data.data()),
                associated_data.size()));

        if (!ciphertext_result.ok()) {
            throw SerdeError("encryption failed: " +
                             std::string(ciphertext_result.status().message()));
        }

        const std::string &result = ciphertext_result.value();
        return std::vector<uint8_t>(result.begin(), result.end());
    }
}

std::vector<uint8_t> Cryptor::decrypt(
    const std::vector<uint8_t> &dek, const std::vector<uint8_t> &ciphertext,
    const std::vector<uint8_t> &associated_data) const {
    google::crypto::tink::KeyData key_data;
    key_data.set_type_url(key_template_.type_url());
    key_data.set_key_material_type(
        google::crypto::tink::KeyData_KeyMaterialType_SYMMETRIC);
    key_data.set_value(std::string(dek.begin(), dek.end()));

    if (isDeterministic()) {
        auto primitive_result = crypto::tink::Registry::GetPrimitive<
            crypto::tink::DeterministicAead>(key_data);
        if (!primitive_result.ok()) {
            throw SerdeError("could not get deterministic aead primitive: " +
                             std::string(primitive_result.status().message()));
        }

        auto primitive = std::move(primitive_result.value());
        auto plaintext_result = primitive->DecryptDeterministically(
            absl::string_view(reinterpret_cast<const char *>(ciphertext.data()),
                              ciphertext.size()),
            absl::string_view(
                reinterpret_cast<const char *>(associated_data.data()),
                associated_data.size()));

        if (!plaintext_result.ok()) {
            throw SerdeError("decryption failed: " +
                             std::string(plaintext_result.status().message()));
        }

        const std::string &result = plaintext_result.value();
        return std::vector<uint8_t>(result.begin(), result.end());
    } else {
        auto primitive_result =
            crypto::tink::Registry::GetPrimitive<crypto::tink::Aead>(key_data);
        if (!primitive_result.ok()) {
            throw SerdeError("could not get aead primitive: " +
                             std::string(primitive_result.status().message()));
        }

        auto primitive = std::move(primitive_result.value());
        auto plaintext_result = primitive->Decrypt(
            absl::string_view(reinterpret_cast<const char *>(ciphertext.data()),
                              ciphertext.size()),
            absl::string_view(
                reinterpret_cast<const char *>(associated_data.data()),
                associated_data.size()));

        if (!plaintext_result.ok()) {
            throw SerdeError("decryption failed: " +
                             std::string(plaintext_result.status().message()));
        }

        const std::string &result = plaintext_result.value();
        return std::vector<uint8_t>(result.begin(), result.end());
    }
}

// EncryptionExecutor implementation
EncryptionExecutor::EncryptionExecutor(std::shared_ptr<Clock> clock)
    : clock_(clock ? clock : std::make_shared<SystemClock>()) {}

void EncryptionExecutor::configure(
    std::shared_ptr<const ClientConfiguration> client_config,
    const std::unordered_map<std::string, std::string> &rule_config) {
    std::lock_guard<std::mutex> client_lock(client_mutex_);

    if (client_) {
        if (*client_->getConfiguration() != *client_config) {
            throw SerdeError("client config already set");
        }
    } else {
        client_ = DekRegistryClient::newClient(client_config);
    }

    std::unique_lock<std::shared_mutex> config_lock(config_mutex_);
    for (const auto &[key, value] : rule_config) {
        auto it = config_.find(key);
        if (it != config_.end() && it->second != value) {
            throw SerdeError("rule config key " + key + " already set");
        }
        config_[key] = value;
    }
}

std::string EncryptionExecutor::getType() const { return "ENCRYPT_PAYLOAD"; }

void EncryptionExecutor::close() {
    std::lock_guard<std::mutex> client_lock(client_mutex_);
    client_.reset();
}

std::unique_ptr<SerdeValue> EncryptionExecutor::transform(
    RuleContext &ctx, const SerdeValue &msg) {
    auto transform = newTransform(ctx);
    return transform->transform(ctx, FieldType::Bytes, msg);
}

IDekRegistryClient *EncryptionExecutor::getClient() const {
    std::lock_guard<std::mutex> lock(client_mutex_);
    return client_.get();
}

void EncryptionExecutor::registerExecutor() {
    global_registry::registerRuleExecutor(
        std::make_shared<EncryptionExecutor>(nullptr));
}

Cryptor EncryptionExecutor::getCryptor(RuleContext &ctx) const {
    schemaregistry::rest::model::Algorithm dek_algorithm =
        schemaregistry::rest::model::Algorithm::Aes256Gcm;

    auto param = ctx.getParameter(ENCRYPT_DEK_ALGORITHM);
    if (param) {
        // Parse algorithm from string
        const std::string &algorithm_str = *param;
        if (algorithm_str == "AES128_GCM") {
            dek_algorithm = schemaregistry::rest::model::Algorithm::Aes128Gcm;
        } else if (algorithm_str == "AES256_GCM") {
            dek_algorithm = schemaregistry::rest::model::Algorithm::Aes256Gcm;
        } else if (algorithm_str == "AES256_SIV") {
            dek_algorithm = schemaregistry::rest::model::Algorithm::Aes256Siv;
        } else {
            throw SerdeError("Unsupported DEK algorithm: " + algorithm_str);
        }
    }

    return Cryptor(dek_algorithm);
}

std::string EncryptionExecutor::getKekName(RuleContext &ctx) const {
    auto param = ctx.getParameter(ENCRYPT_KEK_NAME);
    if (!param || param->empty()) {
        throw SerdeError("no kek name found");
    }
    return *param;
}

int64_t EncryptionExecutor::getDekExpiryDays(RuleContext &ctx) const {
    auto param = ctx.getParameter(ENCRYPT_DEK_EXPIRY_DAYS);
    if (!param) {
        return 0;
    }

    try {
        int64_t days = std::stoll(*param);
        if (days < 0) {
            throw SerdeError("negative expiry days");
        }
        return days;
    } catch (const std::exception &) {
        throw SerdeError("invalid expiry days");
    }
}

std::unique_ptr<EncryptionExecutorTransform> EncryptionExecutor::newTransform(
    RuleContext &ctx) const {
    auto cryptor = getCryptor(ctx);
    auto kek_name = getKekName(ctx);
    auto dek_expiry_days = getDekExpiryDays(ctx);

    return std::make_unique<EncryptionExecutorTransform>(
        this, std::move(cryptor), kek_name, dek_expiry_days);
}

// EncryptionExecutorTransform implementation
EncryptionExecutorTransform::EncryptionExecutorTransform(
    const EncryptionExecutor *executor, Cryptor cryptor,
    const std::string &kek_name, int64_t dek_expiry_days)
    : executor_(executor),
      cryptor_(std::move(cryptor)),
      kek_name_(kek_name),
      dek_expiry_days_(dek_expiry_days) {}

std::unique_ptr<SerdeValue> EncryptionExecutorTransform::transform(
    RuleContext &ctx, FieldType field_type, const SerdeValue &field_value) {
    Mode rule_mode = ctx.getRuleMode();

    switch (rule_mode) {
        case Mode::Write: {
            auto plaintext = toBytes(field_type, field_value);
            if (!plaintext) {
                throw SerdeError("unsupported field type");
            }

            std::optional<int32_t> version;
            if (isDekRotated()) {
                version = -1;
            }

            auto dek = getOrCreateDek(ctx, version);
            auto key_material_bytes = dek.getKeyMaterialBytes();
            if (!key_material_bytes) {
                throw SerdeError("no key material found");
            }

            std::vector<uint8_t> empty_aad;
            auto ciphertext =
                cryptor_.encrypt(*key_material_bytes, *plaintext, empty_aad);

            if (isDekRotated()) {
                ciphertext = prefixVersion(dek.getVersion(), ciphertext);
            }

            if (field_type == FieldType::String) {
                std::string encrypted_value_str =
                    absl::Base64Escape(absl::string_view(
                        reinterpret_cast<const char *>(ciphertext.data()),
                        ciphertext.size()));
                return SerdeValue::newString(
                    ctx.getSerializationContext().serde_format,
                    encrypted_value_str);
            } else {
                return SerdeValue::newBytes(
                    ctx.getSerializationContext().serde_format, ciphertext);
            }
        }

        case Mode::Read: {
            std::optional<std::vector<uint8_t>> ciphertext;

            if (field_type == FieldType::String) {
                auto str_value = field_value.asString();
                std::string decoded;
                if (!absl::Base64Unescape(str_value, &decoded)) {
                    throw SerdeError("could not decode base64 ciphertext");
                }
                ciphertext =
                    std::vector<uint8_t>(decoded.begin(), decoded.end());
            } else {
                ciphertext = toBytes(field_type, field_value);
            }

            if (!ciphertext) {
                return field_value.clone();
            }

            std::optional<int32_t> version;
            if (isDekRotated()) {
                auto [v, c] = extractVersion(*ciphertext);
                if (!v) {
                    throw SerdeError("no version found");
                }
                version = v;
                ciphertext = std::move(c);
            }

            auto dek = getOrCreateDek(ctx, version);
            auto key_material_bytes = dek.getKeyMaterialBytes();

            std::vector<uint8_t> empty_aad;
            auto plaintext =
                cryptor_.decrypt(*key_material_bytes, *ciphertext, empty_aad);

            auto result = toObject(ctx, field_type, plaintext);
            return result ? std::move(result) : field_value.clone();
        }

        default:
            throw SerdeError("unsupported rule mode");
    }
}

bool EncryptionExecutorTransform::isDekRotated() const {
    return dek_expiry_days_ > 0;
}

schemaregistry::rest::model::Kek EncryptionExecutorTransform::getKek(
    RuleContext &ctx) {
    std::lock_guard<std::mutex> lock(kek_mutex_);

    if (kek_) {
        return *kek_;
    }

    auto kek = getOrCreateKek(ctx);
    kek_ = kek;
    return kek;
}

schemaregistry::rest::model::Kek EncryptionExecutorTransform::getOrCreateKek(
    RuleContext &ctx) {
    bool is_read = ctx.getRuleMode() == Mode::Read;
    auto kms_type = ctx.getParameter(ENCRYPT_KMS_TYPE);
    auto kms_key_id = ctx.getParameter(ENCRYPT_KMS_KEY_ID);

    KekId kek_id;
    kek_id.name = kek_name_;
    kek_id.deleted = false;

    auto kek = retrieveKekFromRegistry(kek_id);
    if (kek.has_value()) {
        if (kms_type && kek->getKmsType() != *kms_type) {
            throw SerdeError(absl::StrFormat(
                "found %s with kms type %s which differs from rule kms type %s",
                kek_name_, kek->getKmsType(), *kms_type));
        }
        if (kms_key_id && kek->getKmsKeyId() != *kms_key_id) {
            throw SerdeError(
                absl::StrFormat("found %s with kms key id %s which differs "
                                "from rule kms key id %s",
                                kek_name_, kek->getKmsKeyId(), *kms_key_id));
        }
        return *kek;
    } else {
        if (is_read) {
            throw SerdeError("no kek found for " + kek_id.name +
                             " during consume");
        }
        if (!kms_type) {
            throw SerdeError("no kms type found for " + kek_id.name +
                             " during produce");
        }
        if (!kms_key_id) {
            throw SerdeError("no kms key id found for " + kek_id.name +
                             " during produce");
        }

        kek = storeKekToRegistry(kek_id, *kms_type, *kms_key_id, false);
        if (!kek.has_value()) {
            // Handle conflicts (409)
            kek = retrieveKekFromRegistry(kek_id);
        }

        if (!kek.has_value()) {
            throw SerdeError("no kek found for " + kek_id.name +
                             " during produce");
        }

        return *kek;
    }
}

std::optional<schemaregistry::rest::model::Kek>
EncryptionExecutorTransform::retrieveKekFromRegistry(const KekId &kek_id) {
    try {
        auto client = executor_->getClient();
        if (!client) {
            throw SerdeError("Client not configured");
        }

        auto kek = client->getKek(kek_id.name, kek_id.deleted);
        return kek;
    } catch (const schemaregistry::rest::RestException &e) {
        if (e.getStatus() == 404) {
            return std::nullopt;
        }
        throw;
    } catch (const std::exception &e) {
        throw;
    }
}

std::optional<schemaregistry::rest::model::Kek>
EncryptionExecutorTransform::storeKekToRegistry(const KekId &kek_id,
                                                const std::string &kms_type,
                                                const std::string &kms_key_id,
                                                bool shared) {
    try {
        auto client = executor_->getClient();
        if (!client) {
            throw SerdeError("Client not configured");
        }

        schemaregistry::rest::model::CreateKekRequest request;
        request.setName(kek_id.name);
        request.setKmsType(kms_type);
        request.setKmsKeyId(kms_key_id);
        request.setShared(shared);

        auto kek = client->registerKek(request);
        return kek;
    } catch (const schemaregistry::rest::RestException &e) {
        if (e.getStatus() == 409) {
            return std::nullopt;
        }
        throw;
    } catch (const std::exception &e) {
        throw;
    }
}

schemaregistry::rest::model::Dek EncryptionExecutorTransform::getOrCreateDek(
    RuleContext &ctx, std::optional<int32_t> version) {
    auto kek = getKek(ctx);
    bool is_read = ctx.getRuleMode() == Mode::Read;

    int32_t actual_version = version.value_or(1);
    if (actual_version == 0) {
        actual_version = 1;
    }

    DekId dek_id;
    dek_id.kek_name = kek.getName();
    dek_id.subject = ctx.getSubject();
    dek_id.version = actual_version;
    dek_id.algorithm = cryptor_.isDeterministic()
                           ? schemaregistry::rest::model::Algorithm::Aes256Siv
                           : schemaregistry::rest::model::Algorithm::Aes256Gcm;
    dek_id.deleted = is_read;

    auto dek = retrieveDekFromRegistry(dek_id);
    bool is_expired = isExpired(ctx, dek);

    if (!dek || is_expired) {
        if (is_read) {
            throw SerdeError("no dek found for " + dek_id.kek_name +
                             " during consume");
        }

        std::optional<std::vector<uint8_t>> encrypted_dek;
        if (!kek.getShared()) {
            auto raw_dek = cryptor_.generateKey();
            encrypted_dek = encryptDek(kek, raw_dek);
        }

        int32_t new_version = is_expired ? dek->getVersion() + 1 : 1;

        try {
            auto new_dek = createDek(dek_id, new_version, encrypted_dek);
            dek = new_dek;
        } catch (const std::exception &e) {
            if (!dek) {
                throw;
            }
            // Use existing DEK if creation failed
        }
    }

    auto key_bytes = dek->getKeyMaterialBytes();
    if (!key_bytes) {
        auto encrypted_dek_bytes = dek->getEncryptedKeyMaterialBytes();
        if (!encrypted_dek_bytes) {
            throw SerdeError("no encrypted key material found");
        }

        auto raw_dek = decryptDek(kek, *encrypted_dek_bytes);
        auto updated_dek =
            updateCachedDek(dek_id.kek_name, dek_id.subject, dek_id.algorithm,
                            dek_id.version, dek_id.deleted, raw_dek);
        dek = updated_dek;
    }

    return *dek;
}

schemaregistry::rest::model::Dek EncryptionExecutorTransform::createDek(
    const DekId &dek_id, int32_t new_version,
    const std::optional<std::vector<uint8_t>> &encrypted_dek) {
    DekId new_dek_id = dek_id;
    new_dek_id.version = new_version;

    auto dek = storeDekToRegistry(new_dek_id, encrypted_dek);
    if (!dek) {
        // Handle conflicts (409)
        dek = retrieveDekFromRegistry(dek_id);
    }

    if (!dek) {
        throw SerdeError("no dek found for " + dek_id.kek_name +
                         " during produce");
    }

    return *dek;
}

std::optional<std::vector<uint8_t>> EncryptionExecutorTransform::encryptDek(
    const schemaregistry::rest::model::Kek &kek,
    const std::vector<uint8_t> &raw_dek) {
    std::shared_lock<std::shared_mutex> config_lock(executor_->config_mutex_);
    auto aead = getAead(executor_->config_, kek);

    std::vector<uint8_t> empty_aad;
    auto encrypted = aead->Encrypt(
        absl::string_view(reinterpret_cast<const char *>(raw_dek.data()),
                          raw_dek.size()),
        absl::string_view(reinterpret_cast<const char *>(empty_aad.data()),
                          empty_aad.size()));

    if (!encrypted.ok()) {
        throw SerdeError("Failed to encrypt DEK: " +
                         std::string(encrypted.status().message()));
    }

    const std::string &result = encrypted.value();
    return std::vector<uint8_t>(result.begin(), result.end());
}

std::vector<uint8_t> EncryptionExecutorTransform::decryptDek(
    const schemaregistry::rest::model::Kek &kek,
    const std::vector<uint8_t> &encrypted_dek) {
    std::shared_lock<std::shared_mutex> config_lock(executor_->config_mutex_);
    auto aead = getAead(executor_->config_, kek);

    std::vector<uint8_t> empty_aad;
    auto decrypted = aead->Decrypt(
        absl::string_view(reinterpret_cast<const char *>(encrypted_dek.data()),
                          encrypted_dek.size()),
        absl::string_view(reinterpret_cast<const char *>(empty_aad.data()),
                          empty_aad.size()));

    if (!decrypted.ok()) {
        throw SerdeError("Failed to decrypt DEK: " +
                         std::string(decrypted.status().message()));
    }

    const std::string &result = decrypted.value();
    return std::vector<uint8_t>(result.begin(), result.end());
}

schemaregistry::rest::model::Dek EncryptionExecutorTransform::updateCachedDek(
    const std::string &kek_name, const std::string &subject,
    std::optional<schemaregistry::rest::model::Algorithm> algorithm,
    std::optional<int32_t> version, bool deleted,
    const std::vector<uint8_t> &key_material_bytes) {
    auto client = executor_->getClient();
    if (!client) {
        throw SerdeError("Client not configured");
    }

    auto dek = client->setDekKeyMaterial(kek_name, subject, algorithm, version,
                                         deleted, key_material_bytes);
    return dek;
}

std::optional<schemaregistry::rest::model::Dek>
EncryptionExecutorTransform::retrieveDekFromRegistry(const DekId &dek_id) {
    try {
        auto client = executor_->getClient();
        if (!client) {
            throw SerdeError("Client not configured");
        }

        auto dek =
            client->getDek(dek_id.kek_name, dek_id.subject, dek_id.algorithm,
                           dek_id.version, dek_id.deleted);
        return dek;
    } catch (const schemaregistry::rest::RestException &e) {
        if (e.getStatus() == 404) {
            return std::nullopt;
        }
        throw;
    } catch (const std::exception &e) {
        throw;
    }
}

std::optional<schemaregistry::rest::model::Dek>
EncryptionExecutorTransform::storeDekToRegistry(
    const DekId &dek_id,
    const std::optional<std::vector<uint8_t>> &encrypted_dek) {
    try {
        auto client = executor_->getClient();
        if (!client) {
            throw SerdeError("Client not configured");
        }

        schemaregistry::rest::model::CreateDekRequest request;
        request.setSubject(dek_id.subject);
        request.setVersion(dek_id.version);
        request.setAlgorithm(dek_id.algorithm);

        if (encrypted_dek) {
            std::string encrypted_dek_str =
                absl::Base64Escape(absl::string_view(
                    reinterpret_cast<const char *>(encrypted_dek->data()),
                    encrypted_dek->size()));
            request.setEncryptedKeyMaterial(encrypted_dek_str);
        }

        auto dek = client->registerDek(dek_id.kek_name, request);
        return dek;
    } catch (const schemaregistry::rest::RestException &e) {
        if (e.getStatus() == 409) {
            return std::nullopt;
        }
        throw;
    } catch (const std::exception &e) {
        throw;
    }
}

bool EncryptionExecutorTransform::isExpired(
    RuleContext &ctx,
    const std::optional<schemaregistry::rest::model::Dek> &dek) const {
    int64_t now = executor_->clock_->now();
    return ctx.getRuleMode() != Mode::Read && dek_expiry_days_ > 0 &&
           dek.has_value() &&
           ((now - dek->getTs()) / MILLIS_IN_DAY) > dek_expiry_days_;
}

std::vector<uint8_t> EncryptionExecutorTransform::prefixVersion(
    int32_t version, const std::vector<uint8_t> &ciphertext) const {
    std::vector<uint8_t> payload;
    payload.push_back(0);  // Magic byte

    // Convert version to big-endian 4 bytes
    uint32_t version_be = htobe32(static_cast<uint32_t>(version));
    const uint8_t *version_bytes =
        reinterpret_cast<const uint8_t *>(&version_be);
    payload.insert(payload.end(), version_bytes, version_bytes + 4);

    payload.insert(payload.end(), ciphertext.begin(), ciphertext.end());
    return payload;
}

std::pair<std::optional<int32_t>, std::vector<uint8_t>>
EncryptionExecutorTransform::extractVersion(
    const std::vector<uint8_t> &ciphertext) const {
    if (ciphertext.size() < 5) {
        return {std::nullopt, ciphertext};
    }

    // Extract version from bytes 1-4 (big-endian)
    uint32_t version_be;
    std::memcpy(&version_be, &ciphertext[1], 4);
    int32_t version = static_cast<int32_t>(be32toh(version_be));

    std::vector<uint8_t> remaining_ciphertext(ciphertext.begin() + 5,
                                              ciphertext.end());
    return {version, remaining_ciphertext};
}

std::optional<std::vector<uint8_t>> EncryptionExecutorTransform::toBytes(
    FieldType field_type, const SerdeValue &value) const {
    switch (field_type) {
        case FieldType::String: {
            auto str_value = value.asString();
            return std::vector<uint8_t>(str_value.begin(), str_value.end());
        }
        case FieldType::Bytes:
            return value.asBytes();
        default:
            return std::nullopt;
    }
}

std::unique_ptr<SerdeValue> EncryptionExecutorTransform::toObject(
    RuleContext &ctx, FieldType field_type,
    const std::vector<uint8_t> &value) const {
    switch (field_type) {
        case FieldType::String: {
            // Convert bytes to string
            std::string str_value(value.begin(), value.end());
            return SerdeValue::newString(
                ctx.getSerializationContext().serde_format, str_value);
        }
        case FieldType::Bytes: {
            return SerdeValue::newBytes(
                ctx.getSerializationContext().serde_format, value);
        }
        default:
            return nullptr;
    }
}

std::unique_ptr<crypto::tink::Aead> EncryptionExecutorTransform::getAead(
    const std::unordered_map<std::string, std::string> &config,
    const schemaregistry::rest::model::Kek &kek) {
    std::string kek_url = kek.getKmsType() + "://" + kek.getKmsKeyId();
    auto kms_client = getKmsClient(config, kek_url);

    if (!kms_client) {
        throw SerdeError("KMS client not available for URL: " + kek_url);
    }

    auto aead_result = kms_client->GetAead(kek_url);
    if (!aead_result.ok()) {
        throw SerdeError("Failed to get AEAD: " +
                         std::string(aead_result.status().message()));
    }

    return std::move(aead_result.value());
}

std::shared_ptr<crypto::tink::KmsClient>
EncryptionExecutorTransform::getKmsClient(
    const std::unordered_map<std::string, std::string> &config,
    const std::string &kek_url) {
    try {
        // Try to get an existing KMS client first
        return schemaregistry::rules::encryption::getKmsClient(kek_url);
    } catch (const std::exception &) {
        // If no existing client, get driver and register a new client
        auto driver = schemaregistry::rules::encryption::getKmsDriver(kek_url);
        return registerKmsClient(driver, config, kek_url);
    }
}

std::shared_ptr<crypto::tink::KmsClient>
EncryptionExecutorTransform::registerKmsClient(
    std::shared_ptr<KmsDriver> kms_driver,
    const std::unordered_map<std::string, std::string> &config,
    const std::string &kek_url) {
    // Create a new KMS client using the provided driver
    auto kms_client = kms_driver->newKmsClient(config, kek_url);

    // Register the client with the encryption registry
    schemaregistry::rules::encryption::registerKmsClient(kms_client);

    return kms_client;
}

}  // namespace schemaregistry::rules::encryption
