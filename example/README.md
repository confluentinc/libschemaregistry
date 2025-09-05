# C++ Schema Registry Client Examples

This directory contains C++ examples that demonstrate how to use the cpp-schema-registry-client library.

## Examples

### 1. Basic Avro Consumer (`AvroConsumer.cpp`)
A simple Kafka consumer that deserializes Avro messages using the Schema Registry.

**Usage:**
```bash
./avro_consumer -b localhost:9092 -g my_group -t my_topic -u http://localhost:8081
```

**Options:**
- `-b, --brokers`: Kafka broker list (default: localhost:9092)
- `-g, --group-id`: Consumer group ID (default: example_consumer_group_id)
- `-t, --topics`: Comma-separated list of topics (required)
- `-u, --url`: Schema Registry URL (required)

### 2. Basic Avro Producer (`AvroProducer.cpp`)
A simple Kafka producer that serializes Avro messages using the Schema Registry.

**Usage:**
```bash
./avro_producer -b localhost:9092 -t my_topic -u http://localhost:8081
```

**Options:**
- `-b, --brokers`: Kafka broker list (default: localhost:9092)
- `-t, --topic`: Destination topic (required)
- `-u, --url`: Schema Registry URL (required)

### 3. Avro Consumer with Encryption (`AvroConsumerEncryption.cpp`)
A Kafka consumer that deserializes and decrypts Avro messages using field-level encryption.

**Usage:**
```bash
./avro_consumer_encryption -b localhost:9092 -g my_group -t my_topic -u http://localhost:8081
```

**Options:**
- Same as basic consumer
- Automatically registers and supports all KMS drivers (AWS, Azure, GCP, HashiCorp Vault, Local)

### 4. Avro Producer with Encryption (`AvroProducerEncryption.cpp`)
A Kafka producer that encrypts and serializes Avro messages using field-level encryption.

**Usage:**
```bash
./avro_producer_encryption -b localhost:9092 -t my_topic -u http://localhost:8081 \
  --kek-name my-kek --kms-type local-kms --kms-key-id my-key
```

**Options:**
- Same as basic producer, plus:
- `--kek-name`: Key Encryption Key name (required)
- `--kms-type`: KMS type (required)
- `--kms-key-id`: KMS key ID (required)

## Building the Examples

### Prerequisites
- C++17 compatible compiler (C++20 is required for data quality rules on Windows)
- CMake 3.22+
- librdkafka development libraries
- cpp-schema-registry-client library
- Tink crypto library (for encryption examples)

### Build Instructions

**Using CMake:**
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -G Ninja -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" -DSCHEMAREGISTRY_BUILD_EXAMPLES=ON
cmake --build build --target example
```

## Schema Definition

All examples use the following Avro schema:

```json
{
  "namespace": "confluent.io.examples.serialization.avro",
  "name": "User",
  "type": "record",
  "fields": [
    {"name": "name", "type": "string", "confluent:tags": [ "PII" ]},
    {"name": "favorite_number", "type": "long"},
    {"name": "favorite_color", "type": "string"}
  ]
}
```

For encryption examples, the `name` field is tagged as PII and will be encrypted automatically.

## KMS Configuration

### Encryption Examples
The encryption examples support multiple KMS providers:

- **AWS KMS**: Set environment variables or pass config
- **Azure KMS**: Configure Azure credentials
- **GCP KMS**: Set up GCP service account
- **HashiCorp Vault**: Configure Vault connection
- **Local KMS**: For testing purposes

### Configuration Example
For AWS KMS, you can set environment variables:
```bash
export AWS_ACCESS_KEY_ID=your_access_key
export AWS_SECRET_ACCESS_KEY=your_secret_key
```

Or pass them via the rule configuration in the code.

## Error Handling

All examples include comprehensive error handling for:
- Kafka connection issues
- Schema Registry connectivity
- Serialization/deserialization errors
- Encryption/decryption errors
- KMS communication failures

## Logging

The examples use:
- Console output for main flow and successful operations
- stderr for errors and warnings
- Kafka's built-in logging for debug information

To enable debug logging, the examples set the log level to DEBUG. You can modify this in the code if needed.
