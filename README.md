# C++ Schema Registry Client Library

A C++ client library for interacting with the
[Confluent Schema Registry](https://github.com/confluentinc/schema-registry).

## The library

`cpp-schema-registry-client` provides a Schema Registry client, along with serdes (serializers/deserializers) for
Avro, Protobuf, and JSON Schema.

### Features

- Support for Avro, Protobuf, and JSON Schema formats
- Data quality rules using Google Common Expression Language (CEL) expressions
- Schema migration rules using JSONata expressions
- Client-side field-level encryption (CSFLE) rules using AWS KMS, Azure Key Vault, Google Cloud KMS, or HashiCorp Vault

This library can be used with [librdkafka](https://github.com/confluentinc/librdkafka) but does not depend on it.

### Serdes

- [`AvroSerializer`] and [`AvroDeserializer`] - serdes that use `avro-cpp`
- [`ProtobufSerializer`] and [`ProtobufDeserializer`] - serdes that use `protobuf`
- [`JsonSerializer`] and [`JsonDeserializer`] - serdes that use `jsoncons`


## Build

### Prerequisites
- C++17 compatible compiler (C++20 is required for data quality rules on Windows)
- CMake 3.22+
- vcpkg

To build, first install [vcpkg](https://github.com/microsoft/vcpkg).  Next, run CMake as follows:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -G Ninja -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
cmake --build build -j
cmake --install build --prefix /usr/local
```

## CMake usage

```cmake
find_package(schemaregistry CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE schemaregistry::schemaregistry)
```

This project installs a CMake package called `schemaregistry`. The installed target is exported under the `schemaregistry::` namespace. In the build tree, an alias with the same namespace is provided for consistency.

## Examples

You can find examples in the [example](https://github.com/rayokota/cpp-schema-registry-client/blob/master/example/README.md) folder.
