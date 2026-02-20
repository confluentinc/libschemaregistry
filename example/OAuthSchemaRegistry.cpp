/**
 * Examples of setting up Schema Registry with OAuth, including
 * optional identity pool and union-of-pools support.
 */

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/rest/OAuthProvider.h"
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

  // Example 2: OAuth Client Credentials (native configuration)
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
    std::cout << "Single pool: Found " << subjects.size() << " subjects" << std::endl;
  }

  // Example 3: No identity pool (auto pool mapping)
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

  // Example 4: Union-of-pools via comma-separated string
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

  // Example 5: Union-of-pools from a vector (if you have a list)
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
    std::cout << "Factory (config map): Found " << subjects.size() << " subjects" << std::endl;
  }

  return 0;
}
