#include <iostream>
#include <map>
#include <memory>
#include <string>

#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/rest/OAuthProvider.h"
#include "schemaregistry/rest/SchemaRegistryClient.h"

using namespace schemaregistry::rest;

/**
 * Example demonstrating two OAuth authentication methods:
 * 1. Static Token Provider
 * 2. OAuth Client Credentials Provider
 */

void example_static_token() {
  std::cout << "\n=== Example 1: Static Token Provider ===" << std::endl;

  // Static token obtained from external source (secrets manager, CI/CD, etc.)
  auto provider = std::make_shared<StaticTokenProvider>(
      "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",  // Pre-obtained token
      "lsrc-12345",                                  // Logical cluster ID
      "pool-abcd"                                    // Identity pool ID
  );

  // Create client configuration
  auto config = std::make_shared<ClientConfiguration>(
      std::vector<std::string>{"https://psrc-xxx.us-east-1.aws.confluent.cloud"});

  // Set OAuth provider
  config->setOAuthProvider(provider);

  // Create Schema Registry client
  auto client = SchemaRegistryClient::newClient(config);

  // Use client - tokens automatically added to requests
  try {
    auto subjects = client->getAllSubjects();
    std::cout << "Found " << subjects.size() << " subjects" << std::endl;
  } catch (const std::exception& e) {
    std::cout << "Error: " << e.what() << std::endl;
  }
}

void example_oauth_client_credentials() {
  std::cout << "\n=== Example 2: OAuth Client Credentials Provider ===" << std::endl;

  // Configure OAuth client credentials flow
  OAuthClientProvider::Config oauth_config;
  oauth_config.client_id = "my-client-id";
  oauth_config.client_secret = "my-client-secret";
  oauth_config.scope = "schema_registry";
  oauth_config.token_endpoint_url = "https://idp.example.com/oauth2/token";
  oauth_config.logical_cluster = "lsrc-12345";
  oauth_config.identity_pool_id = "pool-abcd";

  // Create OAuth provider
  auto provider = std::make_shared<OAuthClientProvider>(oauth_config);

  // Create client configuration
  auto config = std::make_shared<ClientConfiguration>(
      std::vector<std::string>{"https://psrc-xxx.us-east-1.aws.confluent.cloud"});

  config->setOAuthProvider(provider);

  // Create Schema Registry client
  auto client = SchemaRegistryClient::newClient(config);

  // Use client - tokens automatically fetched and refreshed!
  try {
    auto subjects = client->getAllSubjects();
    std::cout << "Found " << subjects.size() << " subjects" << std::endl;

    // Token will be automatically refreshed when it reaches 80% of lifetime
    // No manual token management needed!
  } catch (const std::exception& e) {
    std::cout << "Error: " << e.what() << std::endl;
  }
}

void example_oauth_provider_factory() {
  std::cout << "\n=== Example 3: OAuth Provider Factory (Config Map) ===" << std::endl;

  // Configuration map (similar to Python/Java Confluent clients)
  std::map<std::string, std::string> config_map = {
      {"bearer.auth.credentials.source", "OAUTHBEARER"},
      {"bearer.auth.client.id", "my-client-id"},
      {"bearer.auth.client.secret", "my-client-secret"},
      {"bearer.auth.scope", "schema_registry"},
      {"bearer.auth.issuer.endpoint.url", "https://idp.example.com/oauth2/token"},
      {"bearer.auth.logical.cluster", "lsrc-12345"},
      {"bearer.auth.identity.pool.id", "pool-abcd"}
  };

  // Create provider from config map
  auto provider = OAuthProviderFactory::create(config_map);

  // Create client configuration
  auto config = std::make_shared<ClientConfiguration>(
      std::vector<std::string>{"https://psrc-xxx.us-east-1.aws.confluent.cloud"});

  config->setOAuthProvider(provider);

  // Create Schema Registry client
  auto client = SchemaRegistryClient::newClient(config);

  std::cout << "Client created with OAuth provider from config map" << std::endl;
}

int main() {
  std::cout << "OAuth Authentication Examples for Schema Registry C++ Client\n";
  std::cout << "===========================================================\n";

  // NOTE: These examples will fail with actual HTTP requests since
  // we're using dummy credentials. In production, use real credentials.

  example_static_token();
  example_oauth_client_credentials();
  example_oauth_provider_factory();

  std::cout << "\nAll examples completed!" << std::endl;

  return 0;
}
