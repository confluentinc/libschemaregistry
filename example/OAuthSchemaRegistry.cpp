/**
 * Examples of setting up Schema Registry with OAuth using static token
 * and Client Credentials flow, including optional identity pool and
 * union-of-pools support.
 */

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/rest/CustomOAuthProvider.h"
#include "schemaregistry/rest/OAuthClientProvider.h"
#include "schemaregistry/rest/UamiOAuthProvider.h"
#include "schemaregistry/rest/SchemaRegistryClient.h"

using namespace schemaregistry::rest;

int main() {
  // Example 1: Static Token
  {
    auto provider = std::make_shared<StaticTokenProvider>(
        "static-token",   // Pre-obtained token
        "lsrc-12345",     // Logical cluster
        "pool-abcd"       // Identity pool
    );

    auto config = std::make_shared<ClientConfiguration>(
        std::vector<std::string>{"https://psrc-123456.us-east-1.aws.confluent.cloud"});
    config->setOAuthProvider(provider);

    auto client = SchemaRegistryClient::newClient(config);
    auto subjects = client->getAllSubjects();
    std::cout << "Static token: Found " << subjects.size() << " subjects" << std::endl;
  }

  // Example 2.1: OAuth Client Credentials (native configuration)
  {
    OAuthClientProvider::Config oauth_config;
    oauth_config.client_id = "client-id";
    oauth_config.client_secret = "client-secret";
    oauth_config.scope = "schema_registry";
    oauth_config.token_endpoint_url = "https://yourauthprovider.com/v1/token";
    oauth_config.logical_cluster = "lsrc-12345";
    oauth_config.identity_pool_id = "pool-abcd";

    auto provider = std::make_shared<OAuthClientProvider>(oauth_config);

    auto config = std::make_shared<ClientConfiguration>(
        std::vector<std::string>{"https://psrc-123456.us-east-1.aws.confluent.cloud"});
    config->setOAuthProvider(provider);

    auto client = SchemaRegistryClient::newClient(config);
    auto subjects = client->getAllSubjects();
    std::cout << "OAuth client credentials: Found " << subjects.size() << " subjects" << std::endl;
  }

  // Example 2.1b: No identity pool (auto pool mapping)
  {
    OAuthClientProvider::Config oauth_config;
    oauth_config.client_id = "client-id";
    oauth_config.client_secret = "client-secret";
    oauth_config.scope = "schema_registry";
    oauth_config.token_endpoint_url = "https://yourauthprovider.com/v1/token";
    oauth_config.logical_cluster = "lsrc-12345";
    // identity_pool_id left empty -- header is omitted, server uses auto pool mapping

    auto provider = std::make_shared<OAuthClientProvider>(oauth_config);

    auto config = std::make_shared<ClientConfiguration>(
        std::vector<std::string>{"https://psrc-123456.us-east-1.aws.confluent.cloud"});
    config->setOAuthProvider(provider);

    auto client = SchemaRegistryClient::newClient(config);
    auto subjects = client->getAllSubjects();
    std::cout << "Auto pool mapping: Found " << subjects.size() << " subjects" << std::endl;
  }

  // Example 2.1c: Union-of-pools via comma-separated string
  {
    OAuthClientProvider::Config oauth_config;
    oauth_config.client_id = "client-id";
    oauth_config.client_secret = "client-secret";
    oauth_config.scope = "schema_registry";
    oauth_config.token_endpoint_url = "https://yourauthprovider.com/v1/token";
    oauth_config.logical_cluster = "lsrc-12345";
    oauth_config.identity_pool_id = "pool-1,pool-2,pool-3";

    auto provider = std::make_shared<OAuthClientProvider>(oauth_config);

    auto config = std::make_shared<ClientConfiguration>(
        std::vector<std::string>{"https://psrc-123456.us-east-1.aws.confluent.cloud"});
    config->setOAuthProvider(provider);

    auto client = SchemaRegistryClient::newClient(config);
    auto subjects = client->getAllSubjects();
    std::cout << "Union of pools (string): Found " << subjects.size() << " subjects" << std::endl;
  }

  // Example 2.1d: Union-of-pools from a vector (if you have a list)
  {
    OAuthClientProvider::Config oauth_config;
    oauth_config.client_id = "client-id";
    oauth_config.client_secret = "client-secret";
    oauth_config.scope = "schema_registry";
    oauth_config.token_endpoint_url = "https://yourauthprovider.com/v1/token";
    oauth_config.logical_cluster = "lsrc-12345";
    oauth_config.set_identity_pool_ids({"pool-1", "pool-2", "pool-3"});

    auto provider = std::make_shared<OAuthClientProvider>(oauth_config);

    auto config = std::make_shared<ClientConfiguration>(
        std::vector<std::string>{"https://psrc-123456.us-east-1.aws.confluent.cloud"});
    config->setOAuthProvider(provider);

    auto client = SchemaRegistryClient::newClient(config);
    auto subjects = client->getAllSubjects();
    std::cout << "Union of pools (vector): Found " << subjects.size() << " subjects" << std::endl;
  }

  // Example 2.2: OAuth Client Credentials (factory pattern)
  {
    std::map<std::string, std::string> config_map = {
        {"bearer.auth.credentials.source", "OAUTHBEARER"},
        {"bearer.auth.client.id", "client-id"},
        {"bearer.auth.client.secret", "client-secret"},
        {"bearer.auth.scope", "schema_registry"},
        {"bearer.auth.issuer.endpoint.url", "https://yourauthprovider.com/v1/token"},
        {"bearer.auth.logical.cluster", "lsrc-12345"},
        {"bearer.auth.identity.pool.id", "pool-abcd"}
    };

    auto provider = OAuthProviderFactory::create(config_map);

    auto config = std::make_shared<ClientConfiguration>(
        std::vector<std::string>{"https://psrc-123456.us-east-1.aws.confluent.cloud"});
    config->setOAuthProvider(provider);

    auto client = SchemaRegistryClient::newClient(config);
    auto subjects = client->getAllSubjects();
    std::cout << "Factory (config map): Found " << subjects.size() << " subjects" << std::endl;
  }

  // Example 6: Azure User-Assigned Managed Identity (UAMI)
  // See UamiOAuthExample.cpp for a runnable version with CLI args.
  {
    UamiOAuthProvider::Config uami_config;
    uami_config.uami_endpoint_query = "?api-version=2018-02-01&resource=https://confluent.cloud&client_id=<managed-identity-client-id>";
    uami_config.logical_cluster = "lsrc-12345";
    uami_config.identity_pool_id = "pool-abcd";

    auto provider = std::make_shared<UamiOAuthProvider>(uami_config);

    auto config = std::make_shared<ClientConfiguration>(
        std::vector<std::string>{"https://psrc-123456.us-east-1.aws.confluent.cloud"});
    config->setOAuthProvider(provider);

    auto client = SchemaRegistryClient::newClient(config);
    auto subjects = client->getAllSubjects();
    std::cout << "UAMI: Found " << subjects.size() << " subjects" << std::endl;
  }

  return 0;
}
