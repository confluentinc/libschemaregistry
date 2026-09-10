include(FetchContent)

# Download GoogleTest if not already present
# See https://google.github.io/googletest/quickstart-cmake.html
FetchContent_Declare(
    googletest
    URL                    https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP true
)
FetchContent_MakeAvailable(googletest)

# Compile project protobuf files
include(../cmake/CompileProtobuf.cmake)
CompileProtobuf(generated_test_proto ${CMAKE_SOURCE_DIR}/proto)

# Collect test sources conditionally based on enabled features
set(TEST_SOURCES
    WildcardMatcherTest.cpp
    OAuthProviderTest.cpp  # OAuth provider tests
    VariantTest.cpp  # Spark/Parquet Variant codec (core, no rules/avro/proto deps)
)  # Always include base tests

# Rules do not force Avro or Protobuf on, so a test that includes either one's headers has to
# be listed under that feature as well - `SCHEMAREGISTRY_WITH_RULES=ON` with
# `SCHEMAREGISTRY_WITH_AVRO=OFF` is a supported configuration. The two tests below that are
# compiled under rules alone carry `#ifdef SCHEMAREGISTRY_TEST_WITH_AVRO` around their Avro
# cases instead (see the compile definition further down); the rest include those headers
# unconditionally at file scope, so for them the guard belongs here.
if(SCHEMAREGISTRY_WITH_RULES)
    list(APPEND TEST_SOURCES AzureKmsAeadTest.cpp)  # Azure KMS AEAD wire-format tests
    list(APPEND TEST_SOURCES CelDecimalTimestampTest.cpp)  # CEL decimal/timestamp + is* tests
    list(APPEND TEST_SOURCES CelVariantTest.cpp)  # CEL variant() / variants.* tests

    # Needs both: Avro serdes headers *and* test/parity.pb.h.
    if(SCHEMAREGISTRY_WITH_AVRO AND SCHEMAREGISTRY_WITH_PROTOBUF)
        list(APPEND TEST_SOURCES ValidationRuleTest.cpp)  # Inline validation rule tests
        list(APPEND TEST_SOURCES CelAvroWriteBackTest.cpp)
    endif()

    # Protobuf only.
    if(SCHEMAREGISTRY_WITH_PROTOBUF)
        list(APPEND TEST_SOURCES CelProtobufContainerWriteBackTest.cpp)
        list(APPEND TEST_SOURCES CelProtobufScalarNarrowingTest.cpp)
        list(APPEND TEST_SOURCES ProtobufFieldWriteBackTest.cpp)
    endif()
endif()

if(SCHEMAREGISTRY_WITH_AVRO)
    list(APPEND TEST_SOURCES AvroTest.cpp)
endif()

if(SCHEMAREGISTRY_WITH_JSON)
    list(APPEND TEST_SOURCES JsonTest.cpp)
endif()

if(SCHEMAREGISTRY_WITH_PROTOBUF)
    list(APPEND TEST_SOURCES ProtoTest.cpp)
    # Add protobuf sources if protobuf is enabled
    if(PROTO_SOURCES)
        list(APPEND TEST_SOURCES ${PROTO_SOURCES})
    endif()
endif()

# Add the test executable
add_executable(schemaregistry-test ${TEST_SOURCES})

# Include directories for the test executable (base includes)
target_include_directories(schemaregistry-test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/internal     # Access internal headers used by tests
)

# Link libraries for the test executable (base dependencies)
target_link_libraries(schemaregistry-test
    PRIVATE
        GTest::gtest_main
        schemaregistry
)

# Conditional includes and linking based on enabled features
if(SCHEMAREGISTRY_WITH_PROTOBUF)
    target_include_directories(schemaregistry-test
        PRIVATE
            $<$<BOOL:${PROTO_INCLUDE_DIR}>:${PROTO_INCLUDE_DIR}>
            ${CMAKE_BINARY_DIR}/generated_proto  # Include main project's generated protos
    )
    target_link_libraries(schemaregistry-test
        PRIVATE
            protobuf::libprotobuf
    )
endif()

if(SCHEMAREGISTRY_WITH_RULES)
    target_link_libraries(schemaregistry-test
        PRIVATE
            tink::static
            # mpdecimal, so a test can call DecimalUtil directly: its internal header
            # includes decimal.hh, which is otherwise only on the library's own include path.
            $<BUILD_INTERFACE:mpdecxx::mpdecxx>
    )
endif()

# CelDecimalTimestampTest is compiled whenever rules are enabled, but its Avro
# marshalling cases need Avro, which rules do not force on. Flag their availability so
# those cases compile only when Avro is present.
if(SCHEMAREGISTRY_WITH_AVRO)
    target_compile_definitions(schemaregistry-test PRIVATE SCHEMAREGISTRY_TEST_WITH_AVRO)
endif()

# Discover and register tests
include(GoogleTest)
gtest_discover_tests(schemaregistry-test)
