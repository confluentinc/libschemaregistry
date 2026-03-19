/**
 * Azure UAMI OAuth Example for Schema Registry
 *
 * Run on an Azure VM with a User-Assigned Managed Identity:
 *   ./uami_oauth_example \
 *       --schema.registry.url=https://psrc-xxxxx.region.provider.confluent.cloud \
 *       --uami.client.id=<managed-identity-client-id> \
 *       --uami.resource=api://<resource-id> \
 *       --bearer.auth.logical.cluster=lsrc-xxxxx \
 *       --bearer.auth.identity.pool.id=pool-xxxxx
 */

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/rest/UamiOAuthProvider.h"
#include "schemaregistry/rest/SchemaRegistryClient.h"

using namespace schemaregistry::rest;

static std::string get_arg(int argc, char* argv[], const std::string& key) {
  std::string prefix = "--" + key + "=";
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind(prefix, 0) == 0) {
      return arg.substr(prefix.size());
    }
  }
  return "";
}

int main(int argc, char* argv[]) {
  std::string sr_url = get_arg(argc, argv, "schema.registry.url");
  std::string uami_client_id = get_arg(argc, argv, "uami.client.id");
  std::string uami_resource = get_arg(argc, argv, "uami.resource");
  std::string logical_cluster = get_arg(argc, argv, "bearer.auth.logical.cluster");
  std::string identity_pool_id = get_arg(argc, argv, "bearer.auth.identity.pool.id");

  if (sr_url.empty() || uami_client_id.empty() || uami_resource.empty() ||
      logical_cluster.empty() || identity_pool_id.empty()) {
    std::cerr << "Usage: " << argv[0] << " \\\n"
              << "    --schema.registry.url=<url> \\\n"
              << "    --uami.client.id=<managed-identity-client-id> \\\n"
              << "    --uami.resource=<resource-uri> \\\n"
              << "    --bearer.auth.logical.cluster=<lsrc-id> \\\n"
              << "    --bearer.auth.identity.pool.id=<pool-id>\n";
    return 1;
  }

  std::string query_url = "?api-version=2018-02-01"
                          "&resource=" + uami_resource +
                          "&client_id=" + uami_client_id;

  UamiOAuthProvider::Config uami_config;
  uami_config.uami_endpoint_query = query_url;
  uami_config.logical_cluster = logical_cluster;
  uami_config.identity_pool_id = identity_pool_id;

  auto provider = std::make_shared<UamiOAuthProvider>(uami_config);

  std::cout << "Fetching UAMI token from Azure IMDS..." << std::endl;
  auto fields = provider->get_bearer_fields();
  std::cout << "Token obtained successfully (length=" << fields.access_token.size() << ")" << std::endl;

  auto config = std::make_shared<ClientConfiguration>(
      std::vector<std::string>{sr_url});
  config->setOAuthProvider(provider);

  auto client = SchemaRegistryClient::newClient(config);

  std::cout << "Calling getAllSubjects()..." << std::endl;
  auto subjects = client->getAllSubjects();
  std::cout << "Found " << subjects.size() << " subjects:" << std::endl;
  for (const auto& s : subjects) {
    std::cout << "  - " << s << std::endl;
  }

  return 0;
}
