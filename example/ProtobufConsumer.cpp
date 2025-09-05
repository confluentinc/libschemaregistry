#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <thread>
#include <vector>
#include <syslog.h>

// Protobuf includes
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

// Kafka C++ library
#include <librdkafka/rdkafka.h>

// Schema Registry Client includes
#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/rest/SchemaRegistryClient.h"
#include "schemaregistry/serdes/SerdeConfig.h"
#include "schemaregistry/serdes/SerdeTypes.h"
#include "schemaregistry/serdes/protobuf/ProtobufDeserializer.h"

// Generated protobuf message
#include "example/example.pb.h"

using namespace schemaregistry::rest;
using namespace schemaregistry::serdes;
using namespace schemaregistry::serdes::protobuf;

class ConsumerRebalanceCallback {
 public:
  static void rebalance_cb(rd_kafka_t* rk, rd_kafka_resp_err_t err,
                           rd_kafka_topic_partition_list_t* partitions,
                           void* opaque) {
    std::cout << "Rebalance: " << rd_kafka_err2str(err) << std::endl;
    switch (err) {
      case RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS:
        rd_kafka_assign(rk, partitions);
        break;
      case RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS:
        rd_kafka_assign(rk, nullptr);
        break;
      default:
        rd_kafka_assign(rk, nullptr);
        break;
    }
  }
};

class ProtobufConsumerExample {
 private:
  std::shared_ptr<ISchemaRegistryClient> client_;
  std::unique_ptr<ProtobufDeserializer<example::Author>> deserializer_;
  rd_kafka_t* consumer_;
  rd_kafka_conf_t* conf_;

 public:
  ProtobufConsumerExample(const std::string& brokers, const std::string& group_id,
                         const std::string& schema_registry_url)
      : consumer_(nullptr), conf_(nullptr) {
    // Create Schema Registry client configuration
    auto client_config = std::make_shared<ClientConfiguration>(
        std::vector<std::string>{schema_registry_url});

    // Create Schema Registry client
    client_ = SchemaRegistryClient::newClient(client_config);

    // Create deserializer configuration
    std::unordered_map<std::string, std::string> rule_config;
    DeserializerConfig deser_config(std::nullopt, false, rule_config);

    // Create Protobuf deserializer
    deserializer_ = std::make_unique<ProtobufDeserializer<example::Author>>(
        client_, nullptr, deser_config);

    // Create Kafka consumer configuration
    conf_ = rd_kafka_conf_new();
    char errstr[512];

    if (rd_kafka_conf_set(conf_, "bootstrap.servers", brokers.c_str(), errstr,
                          sizeof(errstr)) != RD_KAFKA_CONF_OK) {
      throw std::runtime_error("Failed to set bootstrap.servers: " +
                               std::string(errstr));
    }

    if (rd_kafka_conf_set(conf_, "group.id", group_id.c_str(), errstr,
                          sizeof(errstr)) != RD_KAFKA_CONF_OK) {
      throw std::runtime_error("Failed to set group.id: " + std::string(errstr));
    }

    if (rd_kafka_conf_set(conf_, "enable.partition.eof", "false", errstr,
                          sizeof(errstr)) != RD_KAFKA_CONF_OK) {
      throw std::runtime_error("Failed to set enable.partition.eof: " +
                               std::string(errstr));
    }

    if (rd_kafka_conf_set(conf_, "session.timeout.ms", "6000", errstr,
                          sizeof(errstr)) != RD_KAFKA_CONF_OK) {
      throw std::runtime_error("Failed to set session.timeout.ms: " +
                               std::string(errstr));
    }

    if (rd_kafka_conf_set(conf_, "enable.auto.commit", "true", errstr,
                          sizeof(errstr)) != RD_KAFKA_CONF_OK) {
      throw std::runtime_error("Failed to set enable.auto.commit: " +
                               std::string(errstr));
    }

    // Consume from the start if no committed offsets
    rd_kafka_conf_set(conf_, "auto.offset.reset", "earliest", nullptr, 0);

    // Set rebalance callbacks
    rd_kafka_conf_set_rebalance_cb(conf_, 
                                   ConsumerRebalanceCallback::rebalance_cb);

    // Create consumer
    consumer_ = rd_kafka_new(RD_KAFKA_CONSUMER, conf_, errstr, sizeof(errstr));
    if (!consumer_) {
      throw std::runtime_error("Failed to create consumer: " + std::string(errstr));
    }

    // librdkafka takes ownership of conf_ on success; avoid double free
    conf_ = nullptr;

    // Route callbacks and events to the consumer poll queue
    rd_kafka_poll_set_consumer(consumer_);

    // Set log level
    rd_kafka_set_log_level(consumer_, LOG_DEBUG);
  }

  ~ProtobufConsumerExample() {
    if (consumer_) {
      rd_kafka_consumer_close(consumer_);
      rd_kafka_destroy(consumer_);
    }
    if (conf_) {
      rd_kafka_conf_destroy(conf_);
    }
  }

  void consume_and_print(const std::vector<std::string>& topics) {
    // Create topic partition list for subscription
    rd_kafka_topic_partition_list_t* topic_list = rd_kafka_topic_partition_list_new(topics.size());
    
    for (const auto& topic : topics) {
      rd_kafka_topic_partition_list_add(topic_list, topic.c_str(), RD_KAFKA_PARTITION_UA);
    }

    // Subscribe to topics
    rd_kafka_resp_err_t err = rd_kafka_subscribe(consumer_, topic_list);
    rd_kafka_topic_partition_list_destroy(topic_list);
    
    if (err) {
      throw std::runtime_error("Failed to subscribe to topics: " + std::string(rd_kafka_err2str(err)));
    }

    std::cout << "Consumer started. Waiting for messages..." << std::endl;

    // Consume messages
    while (true) {
      rd_kafka_message_t* msg = rd_kafka_consumer_poll(consumer_, 1000);
      
      if (!msg) {
        continue;
      }

      if (msg->err) {
        if (msg->err != RD_KAFKA_RESP_ERR__PARTITION_EOF) {
          std::cerr << "Consumer error: " << rd_kafka_message_errstr(msg) << std::endl;
        }
        rd_kafka_message_destroy(msg);
        continue;
      }

      // Create serialization context
      SerializationContext ser_ctx;
      ser_ctx.topic = std::string(static_cast<const char*>(rd_kafka_topic_name(msg->rkt)));
      ser_ctx.serde_type = SerdeType::Value;
      ser_ctx.serde_format = SerdeFormat::Protobuf;
      ser_ctx.headers = std::nullopt;

      try {
        // Deserialize the message
        std::vector<uint8_t> payload(static_cast<const uint8_t*>(msg->payload),
                                   static_cast<const uint8_t*>(msg->payload) + msg->len);
        
        auto author_ptr = deserializer_->deserialize(ser_ctx, payload);
        const example::Author& author = *author_ptr;

        // Convert to JSON for display
        std::string json_string;
        google::protobuf::util::JsonPrintOptions options;
        options.add_whitespace = true;
        options.always_print_fields_with_no_presence = true;
        auto status = google::protobuf::util::MessageToJsonString(author, &json_string, options);
        if (!status.ok()) {
          std::cerr << "Error converting to JSON: " << status << std::endl;
          continue;
        }

        std::cout << "Received message:" << std::endl;
        std::cout << "  Topic: " << ser_ctx.topic << std::endl;
        std::cout << "  Partition: " << msg->partition << std::endl;
        std::cout << "  Offset: " << msg->offset << std::endl;
        std::cout << "  Payload: " << json_string << std::endl;

        // Commit the message
        err = rd_kafka_commit_message(consumer_, msg, 1);
        if (err) {
          std::cerr << "Failed to commit message: " << rd_kafka_err2str(err) << std::endl;
        }

      } catch (const std::exception& e) {
        std::cerr << "Error deserializing message: " << e.what() << std::endl;
      }

      rd_kafka_message_destroy(msg);
    }
  }
};

void print_usage(const char* program_name) {
  std::cout << "Usage: " << program_name << " [OPTIONS]" << std::endl;
  std::cout << "Options:" << std::endl;
  std::cout << "  -b, --brokers <brokers>           Broker list in kafka format (default: localhost:9092)" << std::endl;
  std::cout << "  -g, --group-id <group-id>         Consumer group id (default: example_consumer_group_id)" << std::endl;
  std::cout << "  -t, --topics <topics>             Topic list (comma-separated, required)" << std::endl;
  std::cout << "  -u, --url <schema-registry-url>   Schema Registry URL (required)" << std::endl;
  std::cout << "  -h, --help                        Show this help message" << std::endl;
}

std::vector<std::string> split_topics(const std::string& topics_str) {
  std::vector<std::string> topics;
  std::stringstream ss(topics_str);
  std::string topic;
  
  while (std::getline(ss, topic, ',')) {
    if (!topic.empty()) {
      topics.push_back(topic);
    }
  }
  
  return topics;
}

int main(int argc, char* argv[]) {
  std::string brokers = "localhost:9092";
  std::string group_id = "example_consumer_group_id";
  std::string topics_str;
  std::string schema_registry_url;

  // Parse command line arguments
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if ((arg == "-b" || arg == "--brokers") && i + 1 < argc) {
      brokers = argv[++i];
    } else if ((arg == "-g" || arg == "--group-id") && i + 1 < argc) {
      group_id = argv[++i];
    } else if ((arg == "-t" || arg == "--topics") && i + 1 < argc) {
      topics_str = argv[++i];
    } else if ((arg == "-u" || arg == "--url") && i + 1 < argc) {
      schema_registry_url = argv[++i];
    } else {
      std::cerr << "Unknown argument: " << arg << std::endl;
      print_usage(argv[0]);
      return 1;
    }
  }

  // Validate required arguments
  if (topics_str.empty()) {
    std::cerr << "Error: topics are required" << std::endl;
    print_usage(argv[0]);
    return 1;
  }

  if (schema_registry_url.empty()) {
    std::cerr << "Error: schema registry URL is required" << std::endl;
    print_usage(argv[0]);
    return 1;
  }

  std::vector<std::string> topics = split_topics(topics_str);
  if (topics.empty()) {
    std::cerr << "Error: no valid topics provided" << std::endl;
    return 1;
  }

  std::cout << "Starting Protobuf consumer with configuration:" << std::endl;
  std::cout << "  Brokers: " << brokers << std::endl;
  std::cout << "  Group ID: " << group_id << std::endl;
  std::cout << "  Topics: " << topics_str << std::endl;
  std::cout << "  Schema Registry URL: " << schema_registry_url << std::endl;

  try {
    ProtobufConsumerExample consumer(brokers, group_id, schema_registry_url);
    consumer.consume_and_print(topics);
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
