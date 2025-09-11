# CMakeLists.txt for Schema Registry Client Examples
# This file is designed to be used as part of the main project build

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(protobuf CONFIG REQUIRED)

# Find librdkafka
pkg_check_modules(RDKAFKA REQUIRED rdkafka)

# Common include directories
include_directories(
    ${RDKAFKA_INCLUDE_DIRS}
)

# Common link directories
link_directories(
    ${RDKAFKA_LIBRARY_DIRS}
)

# Compile project protobuf files
include(../cmake/CompileProtobuf.cmake)
CompileProtobuf(generated_example_proto ${CMAKE_SOURCE_DIR}/proto)

# Function to create example executable
function(add_example_executable target_name source_file)
    add_executable(${target_name} ${source_file})
    
    target_compile_options(${target_name} PRIVATE ${RDKAFKA_CFLAGS_OTHER})
    
    # Add protobuf include directory for protobuf examples
    if(${target_name} MATCHES "protobuf_.*")
        target_include_directories(${target_name} PRIVATE
            ${PROTO_INCLUDE_DIR}
            ${CMAKE_BINARY_DIR}/generated_proto  # Include main project's generated protos
        )
        target_sources(${target_name} PRIVATE ${PROTO_SOURCES})
        target_link_libraries(${target_name}
            schemaregistry
            ${RDKAFKA_LIBRARIES}
            protobuf::libprotobuf
            ${ARGN}  # Additional libraries passed as arguments
        )
    else()
        target_link_libraries(${target_name}
            schemaregistry
            ${RDKAFKA_LIBRARIES}
            ${ARGN}  # Additional libraries passed as arguments
        )
    endif()
    
    # Add to example target
    add_dependencies(example ${target_name})
endfunction()

# Create example target
add_custom_target(example)

# Build list of example targets dynamically
set(EXAMPLE_TARGETS)

# Only build Avro examples if Avro support is enabled
if(SCHEMAREGISTRY_WITH_AVRO)
    add_example_executable(avro_consumer AvroConsumer.cpp)
    add_example_executable(avro_producer AvroProducer.cpp)
    list(APPEND EXAMPLE_TARGETS avro_consumer avro_producer)
    
    # Only build Avro encryption examples if Rules support is also enabled
    if(SCHEMAREGISTRY_WITH_RULES)
        add_example_executable(avro_consumer_encryption AvroConsumerEncryption.cpp
            tink::static
        )
        add_example_executable(avro_producer_encryption AvroProducerEncryption.cpp
            tink::static
        )
        list(APPEND EXAMPLE_TARGETS avro_consumer_encryption avro_producer_encryption)
    endif()
endif()

# Only build JSON examples if JSON support is enabled
if(SCHEMAREGISTRY_WITH_JSON)
    add_example_executable(json_consumer JsonConsumer.cpp)
    add_example_executable(json_producer JsonProducer.cpp)
    list(APPEND EXAMPLE_TARGETS json_consumer json_producer)
    
    # Only build JSON encryption examples if Rules support is also enabled
    if(SCHEMAREGISTRY_WITH_RULES)
        add_example_executable(json_consumer_encryption JsonConsumerEncryption.cpp
            tink::static
        )
        add_example_executable(json_producer_encryption JsonProducerEncryption.cpp
            tink::static
        )
        list(APPEND EXAMPLE_TARGETS json_consumer_encryption json_producer_encryption)
    endif()
endif()

# Only build Protobuf examples if Protobuf support is enabled
if(SCHEMAREGISTRY_WITH_PROTOBUF)
    add_example_executable(protobuf_consumer ProtobufConsumer.cpp)
    add_example_executable(protobuf_producer ProtobufProducer.cpp)
    list(APPEND EXAMPLE_TARGETS protobuf_consumer protobuf_producer)
    
    # Only build Protobuf encryption examples if Rules support is also enabled
    if(SCHEMAREGISTRY_WITH_RULES)
        add_example_executable(protobuf_consumer_encryption ProtobufConsumerEncryption.cpp
            tink::static
        )
        add_example_executable(protobuf_producer_encryption ProtobufProducerEncryption.cpp
            tink::static
        )
        list(APPEND EXAMPLE_TARGETS protobuf_consumer_encryption protobuf_producer_encryption)
    endif()
endif()

# Protobuf compilation and dependencies are handled in add_example_executable function

# Set output directory for examples (only if examples were built)
if(EXAMPLE_TARGETS)
    set_target_properties(${EXAMPLE_TARGETS}
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/example
    )

    # Add install targets for built examples
    install(TARGETS ${EXAMPLE_TARGETS}
        DESTINATION bin/example
    )
endif()

# Install README
install(FILES README.md DESTINATION share/doc/schemaregistry-examples)

# Display information
if(EXAMPLE_TARGETS)
    message(STATUS "Schema Registry Examples will be built in: ${CMAKE_BINARY_DIR}/example")
    message(STATUS "Enabled examples: ${EXAMPLE_TARGETS}")
    message(STATUS "Run 'make example' to build all examples")
else()
    message(STATUS "No examples will be built (all serialization formats disabled)")
endif()
