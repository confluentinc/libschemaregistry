/**
 * MockDekRegistryClient
 * Mock implementation of Data Encryption Key (DEK) Registry Client for testing
 */

#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/rest/DekRegistryTypes.h"
#include "schemaregistry/rest/IDekRegistryClient.h"
#include "schemaregistry/rest/RestException.h"

namespace schemaregistry::rest {

// Forward declarations
class DekStore;

/**
 * Mock Store for caching DEK and KEK data
 */
class MockDekStore {
  public:
    MockDekStore();
    ~MockDekStore() = default;

    void setKek(const KekId &kekId,
                const schemaregistry::rest::model::Kek &kek);
    void setDek(const DekId &dekId,
                const schemaregistry::rest::model::Dek &dek);

    std::optional<schemaregistry::rest::model::Kek> getKek(
        const KekId &kekId) const;
    std::optional<schemaregistry::rest::model::Dek> getDek(
        const DekId &dekId) const;

    schemaregistry::rest::model::Dek *getMutDek(const DekId &dekId);

    void clear();

  private:
    std::unordered_map<KekId, schemaregistry::rest::model::Kek> keks;
    std::unordered_map<DekId, schemaregistry::rest::model::Dek> deks;

    friend class MockDekRegistryClient;
};

/**
 * Mock DEK Registry Client implementation for testing
 */
class MockDekRegistryClient : public IDekRegistryClient {
  public:
    explicit MockDekRegistryClient(
        std::shared_ptr<const schemaregistry::rest::ClientConfiguration>
            config);

    virtual ~MockDekRegistryClient() = default;

    virtual std::shared_ptr<const schemaregistry::rest::ClientConfiguration>
    getConfiguration() const override;

    // IDekRegistryClient implementation
    virtual schemaregistry::rest::model::Kek registerKek(
        const schemaregistry::rest::model::CreateKekRequest &request) override;

    virtual schemaregistry::rest::model::Dek registerDek(
        const std::string &kek_name,
        const schemaregistry::rest::model::CreateDekRequest &request) override;

    virtual schemaregistry::rest::model::Kek getKek(
        const std::string &name, bool deleted = false) override;

    virtual schemaregistry::rest::model::Dek getDek(
        const std::string &kek_name, const std::string &subject,
        const std::optional<schemaregistry::rest::model::Algorithm> &algorithm =
            std::nullopt,
        const std::optional<int32_t> &version = std::nullopt,
        bool deleted = false) override;

    virtual schemaregistry::rest::model::Dek setDekKeyMaterial(
        const std::string &kek_name, const std::string &subject,
        const std::optional<schemaregistry::rest::model::Algorithm> &algorithm =
            std::nullopt,
        const std::optional<int32_t> &version = std::nullopt,
        bool deleted = false,
        const std::vector<uint8_t> &key_material_bytes = {}) override;

    virtual void clearCaches() override;

    virtual void close() override;

  private:
    std::shared_ptr<const schemaregistry::rest::ClientConfiguration> config;
    std::shared_ptr<MockDekStore> store;
    std::shared_ptr<std::mutex> storeMutex;

    // Helper methods
    int64_t getCurrentTimestamp() const;
};

}  // namespace schemaregistry::rest