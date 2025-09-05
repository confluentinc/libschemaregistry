#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Protobuf includes
#include <google/protobuf/message.h>

// Kafka C++ library
#include <librdkafka/rdkafka.h>

// Schema Registry Client includes
#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/rest/SchemaRegistryClient.h"
#include "schemaregistry/rest/model/Schema.h"
#include "schemaregistry/serdes/SerdeConfig.h"
#include "schemaregistry/serdes/SerdeTypes.h"
#include "schemaregistry/serdes/protobuf/ProtobufSerializer.h"

// Generated protobuf message
#include "example/example.pb.h"

using namespace schemaregistry::rest;
using namespace schemaregistry::rest::model;
using namespace schemaregistry::serdes;
using namespace schemaregistry::serdes::protobuf;

class ProtobufProducerExample {
 private:
  std::shared_ptr<ISchemaRegistryClient> client_;
  std::unique_ptr<ProtobufSerializer<example::Author>> serializer_;
  rd_kafka_t* producer_;
  rd_kafka_conf_t* conf_;

 public:
  ProtobufProducerExample(const std::string& brokers,
                         const std::string& schema_registry_url)
      : producer_(nullptr), conf_(nullptr) {
    // Create Schema Registry client configuration
    auto client_config = std::make_shared<ClientConfiguration>(
        std::vector<std::string>{schema_registry_url});

    // Create Schema Registry client
    client_ = SchemaRegistryClient::newClient(client_config);

    // Create serializer configuration
    std::unordered_map<std::string, std::string> rule_config;
    SerializerConfig ser_config(true, std::nullopt, true, false, rule_config);

    // Create Protobuf serializer
    serializer_ = std::make_unique<ProtobufSerializer<example::Author>>(
        client_, std::nullopt, nullptr, ser_config);

    // Create Kafka producer configuration
    conf_ = rd_kafka_conf_new();
    char errstr[512];

    if (rd_kafka_conf_set(conf_, "bootstrap.servers", brokers.c_str(), errstr,
                          sizeof(errstr)) != RD_KAFKA_CONF_OK) {
      throw std::runtime_error("Failed to set bootstrap.servers: " +
                               std::string(errstr));
    }

    if (rd_kafka_conf_set(conf_, "message.timeout.ms", "5000", errstr,
                          sizeof(errstr)) != RD_KAFKA_CONF_OK) {
      throw std::runtime_error("Failed to set message.timeout.ms: " +
                               std::string(errstr));
    }

    // Create producer
    producer_ = rd_kafka_new(RD_KAFKA_PRODUCER, conf_, errstr, sizeof(errstr));
    if (!producer_) {
      throw std::runtime_error("Failed to create producer: " + std::string(errstr));
    }
    // librdkafka takes ownership of conf_ on success; avoid double free
    conf_ = nullptr;
  }

  ~ProtobufProducerExample() {
    if (producer_) {
      // Wait for any outstanding messages to be delivered
      rd_kafka_flush(producer_, 10000);
      rd_kafka_destroy(producer_);
    }
    if (conf_) {
      rd_kafka_conf_destroy(conf_);
    }
  }

  void produce_messages(const std::string& topic_name) {
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = topic_name;
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Protobuf;
    ser_ctx.headers = std::nullopt;

    std::cout << "Producing 5 messages to topic: " << topic_name << std::endl;

    // Produce 5 messages
    for (int i = 0; i < 5; i++) {
      try {
        // Create Author object with example data
        example::Author obj;
        obj.set_name("Name " + std::to_string(i));
        obj.set_id(i);
        obj.set_picture(std::string{1, 2, 3}); // bytes field
        obj.add_works("Metamorphosis");
        obj.add_works("The Trial");
        obj.set_oneof_string("oneof");

        // Serialize the protobuf object
        std::vector<uint8_t> serialized_data = serializer_->serialize(ser_ctx, obj);

        // Produce the message
        rd_kafka_resp_err_t err = rd_kafka_producev(
            producer_,
            RD_KAFKA_V_TOPIC(topic_name.c_str()),
            RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
            RD_KAFKA_V_VALUE(serialized_data.data(), serialized_data.size()),
            RD_KAFKA_V_END);

        if (err) {
          std::cerr << "Failed to produce message " << i << ": " << rd_kafka_err2str(err) << std::endl;
        } else {
          std::cout << "Enqueued message " << i << " for delivery" << std::endl;
        }

        // Poll for delivery reports
        rd_kafka_poll(producer_, 0);

      } catch (const std::exception& e) {
        std::cerr << "Error producing message " << i << ": " << e.what() << std::endl;
      }
    }

    // Wait for all messages to be delivered
    std::cout << "Waiting for message delivery..." << std::endl;
    rd_kafka_flush(producer_, 10000);
    
    // Final poll to handle any remaining delivery reports
    rd_kafka_poll(producer_, 1000);
  }
};

void print_usage(const char* program_name) {
  std::cout << "Usage: " << program_name << " [OPTIONS]" << std::endl;
  std::cout << "Options:" << std::endl;
  std::cout << "  -b, --brokers <brokers>           Broker list in kafka format (default: localhost:9092)" << std::endl;
  std::cout << "  -t, --topic <topic>               Destination topic (required)" << std::endl;
  std::cout << "  -u, --url <schema-registry-url>   Schema Registry URL (required)" << std::endl;
  std::cout << "  -h, --help                        Show this help message" << std::endl;
}

int main(int argc, char* argv[]) {
  std::string brokers = "localhost:9092";
  std::string topic;
  std::string schema_registry_url;

  // Parse command line arguments
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if ((arg == "-b" || arg == "--brokers") && i + 1 < argc) {
      brokers = argv[++i];
    } else if ((arg == "-t" || arg == "--topic") && i + 1 < argc) {
      topic = argv[++i];
    } else if ((arg == "-u" || arg == "--url") && i + 1 < argc) {
      schema_registry_url = argv[++i];
    } else {
      std::cerr << "Unknown argument: " << arg << std::endl;
      print_usage(argv[0]);
      return 1;
    }
  }

  // Validate required arguments
  if (topic.empty()) {
    std::cerr << "Error: topic is required" << std::endl;
    print_usage(argv[0]);
    return 1;
  }

  if (schema_registry_url.empty()) {
    std::cerr << "Error: schema registry URL is required" << std::endl;
    print_usage(argv[0]);
    return 1;
  }

  std::cout << "Starting Protobuf producer with configuration:" << std::endl;
  std::cout << "  Brokers: " << brokers << std::endl;
  std::cout << "  Topic: " << topic << std::endl;
  std::cout << "  Schema Registry URL: " << schema_registry_url << std::endl;

  try {
    ProtobufProducerExample producer(brokers, schema_registry_url);
    producer.produce_messages(topic);
    
    std::cout << "All messages sent successfully!" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
