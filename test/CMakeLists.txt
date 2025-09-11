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
set(TEST_SOURCES WildcardMatcherTest.cpp)  # Always include base tests

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
    )
endif()

# Discover and register tests
include(GoogleTest)
gtest_discover_tests(schemaregistry-test)
