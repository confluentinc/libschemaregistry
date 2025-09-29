cmake_minimum_required(VERSION 3.22)

# Set policies for better compatibility
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)  # Set timestamp policy for URL downloads
endif()

project(schemaregistry VERSION 0.1.0 LANGUAGES CXX)

# Enforce C++17 globally so all fetched dependencies (e.g., ANTLR4 runtime)
# are built with C++17 features available (std::any, std::shared_mutex, etc.).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SCHEMAREGISTRY_WITH_AVRO "Build with Avro support" ON)
option(SCHEMAREGISTRY_WITH_JSON "Build with JSON Schema support" ON)
option(SCHEMAREGISTRY_WITH_PROTOBUF "Build with Protobuf support" ON)
option(SCHEMAREGISTRY_WITH_RULES "Build with Data Contract rules support" ON)

if(VCPKG_MANIFEST_FEATURES)
    if(NOT DEFINED SCHEMAREGISTRY_WITH_AVRO)
        if(NOT "avro" IN_LIST VCPKG_MANIFEST_FEATURES)
            set(SCHEMAREGISTRY_WITH_AVRO OFF)
        endif()
    endif()
    
    if(NOT DEFINED SCHEMAREGISTRY_WITH_JSON)
        if(NOT "json" IN_LIST VCPKG_MANIFEST_FEATURES)
            set(SCHEMAREGISTRY_WITH_JSON OFF)
        endif()
    endif()
    
    if(NOT DEFINED SCHEMAREGISTRY_WITH_PROTOBUF)
        if(NOT "protobuf" IN_LIST VCPKG_MANIFEST_FEATURES)
            set(SCHEMAREGISTRY_WITH_PROTOBUF OFF)
        endif()
    endif()

    if(NOT DEFINED SCHEMAREGISTRY_WITH_RULES)
        if(NOT "rules" IN_LIST VCPKG_MANIFEST_FEATURES)
            set(SCHEMAREGISTRY_WITH_RULES OFF)
        endif()
    endif()
endif()

if(SCHEMAREGISTRY_WITH_RULES)
    if(NOT SCHEMAREGISTRY_WITH_PROTOBUF)
        message(WARNING "Rules require Protobuf support. Auto-enabling Protobuf.")
        set(SCHEMAREGISTRY_WITH_PROTOBUF ON)
    endif()
endif()

include(FetchContent)

if(WIN32)
    if(SCHEMAREGISTRY_WITH_RULES)
        # Bump C++ standard to 20 on Windows when building with CEL rules
        message(STATUS "Using C++20 on Windows because CEL rules are enabled")
        set(CMAKE_CXX_STANDARD 20)
    endif()
    # Use a shorter base directory to avoid path length issues
    set(FETCHCONTENT_BASE_DIR C:/deps)
    # Avoid conflicts with min/max macros in Windows headers
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

find_package(absl CONFIG REQUIRED)

find_package(OpenSSL CONFIG REQUIRED)

# Find cpr
find_package(cpr CONFIG REQUIRED)

if (SCHEMAREGISTRY_WITH_AVRO)
    # Fetch Apache Avro C++
    find_package(unofficial-avro-cpp CONFIG QUIET)
    if(NOT unofficial-avro-cpp_FOUND)
    FetchContent_Declare(
        unofficial-avro-cpp
        GIT_REPOSITORY https://github.com/apache/avro.git
        GIT_TAG        release-1.12.0
        EXCLUDE_FROM_ALL
        SOURCE_SUBDIR  lang/c++
    )
    # Disable Avro subproject tests to avoid registering them with ctest here
    set(AVROCPP_BUILD_TESTS OFF CACHE BOOL "Build Avro C++ tests" FORCE)
    set(AVRO_BUILD_TESTS OFF CACHE BOOL "Build Avro tests" FORCE)
    FetchContent_MakeAvailable(unofficial-avro-cpp)
    endif()
endif()

# Find nlohmann_json or fetch if not found
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.12.0
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

if (SCHEMAREGISTRY_WITH_JSON)
    # Fetch jsoncons
    find_package(jsoncons CONFIG QUIET)
    if(NOT jsoncons_FOUND)
        FetchContent_Declare(
            jsoncons
            GIT_REPOSITORY https://github.com/danielaparker/jsoncons.git
            GIT_TAG        v1.3.2
        )
        # Disable jsoncons unit tests
        set(JSONCONS_BUILD_TESTS OFF CACHE BOOL "Build jsoncons tests" FORCE)
        FetchContent_MakeAvailable(jsoncons)
    endif()
endif()

if (SCHEMAREGISTRY_WITH_RULES)
    # Fetch jsonata
    FetchContent_Declare(
        jsonata
        URL https://github.com/rayokota/jsonata-cpp/archive/refs/tags/v0.1.0.zip
        URL_HASH SHA256=3ee1798f28a29d36ebbb273853979926716a384e4d491a6bd408e1f6de51760d
    )
    # Disable jsonata unit tests
    set(JSONATA_BUILD_TESTS OFF CACHE BOOL "Build jsonata tests" FORCE)
    FetchContent_MakeAvailable(jsonata)
endif()

if (SCHEMAREGISTRY_WITH_PROTOBUF)
    # Find Protobuf for protoc executable and libraries
    find_package(protobuf CONFIG REQUIRED)
endif()

if (SCHEMAREGISTRY_WITH_RULES)
    # Note that cel-cpp has no CMake build and is not packaged anywhere,
    # so we build protovalidate_cc as an in-tree dependency to get cel-cpp.
    FetchContent_Declare(
        protovalidate_cc
        GIT_REPOSITORY https://github.com/bufbuild/protovalidate-cc.git
        GIT_TAG        v1.0.0
    )
    FetchContent_MakeAvailable(protovalidate_cc)
endif()

if (SCHEMAREGISTRY_WITH_RULES)
    # AWS SDK C++
    find_package(aws-cpp-sdk-core CONFIG REQUIRED)
    find_package(aws-cpp-sdk-kms CONFIG REQUIRED)
    find_package(aws-cpp-sdk-sts CONFIG REQUIRED)
    find_package(aws-cpp-sdk-cognito-identity CONFIG REQUIRED)
    find_package(aws-cpp-sdk-identity-management CONFIG REQUIRED)
endif()

if (SCHEMAREGISTRY_WITH_RULES)
    # Azure Key Vault C++ SDK
    find_package(azure-security-keyvault-keys-cpp CONFIG REQUIRED)
    find_package(azure-identity-cpp CONFIG REQUIRED)
endif()

if (SCHEMAREGISTRY_WITH_RULES)
    # Google Cloud C++ SDK
    find_package(google_cloud_cpp_kms CONFIG REQUIRED)
endif()

if (SCHEMAREGISTRY_WITH_RULES)
    # HashiCorp Vault C++ client
    find_package(libvault CONFIG REQUIRED)
endif()

if (SCHEMAREGISTRY_WITH_RULES)
    # Import Tink as an in-tree dependency.
    # See https://developers.google.com/tink/setup/cc
    # Configure Tink to use system-provided dependencies instead of building its own
    set(TINK_USE_SYSTEM_OPENSSL ON CACHE BOOL "Use system OpenSSL" FORCE)
    set(TINK_USE_INSTALLED_ABSEIL ON CACHE BOOL "Use installed Abseil" FORCE)
    set(TINK_USE_INSTALLED_PROTOBUF ON CACHE BOOL "Use installed Protobuf" FORCE)
    set(USE_ONLY_FIPS OFF CACHE BOOL "Use only FIPS" FORCE)
    FetchContent_Declare(
        tink
        URL       https://github.com/tink-crypto/tink-cc/archive/refs/tags/v2.4.0.zip
        URL_HASH  SHA256=06c4d49b0b1357f0b8c3abc77a7d920130dc868e4597d432a9ce1cda4f65e382
    )
    FetchContent_MakeAvailable(tink)
endif()

# Compile project protobuf files
include(cmake/CompileProtobuf.cmake)
CompileProtobuf(generated_proto)

# Create the main library
add_library(schemaregistry)

# Base sources (always included - REST client and core functionality)
file(GLOB_RECURSE BASE_HEADERS "include/schemaregistry/rest/*.h")
file(GLOB_RECURSE BASE_SOURCES "src/rest/*.cpp")
target_sources(schemaregistry PRIVATE ${BASE_HEADERS} ${BASE_SOURCES})

# Core serdes sources (always needed - base serialization framework)
# Also include JsonValue as it is used for schema migration rules
file(GLOB CORE_SERDES_HEADERS "include/schemaregistry/serdes/Serde.h" 
                              "include/schemaregistry/serdes/SerdeConfig.h"
                              "include/schemaregistry/serdes/SerdeError.h"
                              "include/schemaregistry/serdes/SerdeTypes.h"
                              "include/schemaregistry/serdes/RuleRegistry.h"
                              "include/schemaregistry/serdes/WildcardMatcher.h"
                              "src/internal/schemaregistry/serdes/json/JsonValue.h")
file(GLOB CORE_SERDES_SOURCES "src/serdes/Serde.cpp"
                              "src/serdes/SerdeConfig.cpp" 
                              "src/serdes/SerdeError.cpp"
                              "src/serdes/SerdeTypes.cpp"
                              "src/serdes/RuleRegistry.cpp"
                              "src/serdes/WildcardMatcher.cpp"
                              "src/serdes/json/JsonValue.cpp")
target_sources(schemaregistry PRIVATE ${CORE_SERDES_HEADERS} ${CORE_SERDES_SOURCES})

# Conditionally add Avro sources
if(SCHEMAREGISTRY_WITH_AVRO)
    file(GLOB AVRO_HEADERS "include/schemaregistry/serdes/avro/*.h")
    file(GLOB AVRO_SOURCES "src/serdes/avro/*.cpp")
    file(GLOB AVRO_INTERNAL_HEADERS "src/internal/schemaregistry/serdes/avro/*.h")
    target_sources(schemaregistry PRIVATE ${AVRO_HEADERS} ${AVRO_SOURCES} ${AVRO_INTERNAL_HEADERS})
endif()

# Conditionally add JSON sources
if(SCHEMAREGISTRY_WITH_JSON)
    file(GLOB JSON_HEADERS "include/schemaregistry/serdes/json/*.h")
    file(GLOB JSON_SOURCES "src/serdes/json/*.cpp")
    file(GLOB JSON_INTERNAL_HEADERS "src/internal/schemaregistry/serdes/json/*.h")
    # Remove JsonValue files as they are now in CORE_SERDES
    list(REMOVE_ITEM JSON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/serdes/json/JsonValue.cpp")
    list(REMOVE_ITEM JSON_INTERNAL_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/schemaregistry/serdes/json/JsonValue.h")
    target_sources(schemaregistry PRIVATE ${JSON_HEADERS} ${JSON_SOURCES} ${JSON_INTERNAL_HEADERS})
endif()

# Conditionally add Protobuf sources
if(SCHEMAREGISTRY_WITH_PROTOBUF)
    file(GLOB PROTOBUF_HEADERS "include/schemaregistry/serdes/protobuf/*.h")
    file(GLOB PROTOBUF_SOURCES "src/serdes/protobuf/*.cpp")
    target_sources(schemaregistry PRIVATE ${PROTOBUF_HEADERS} ${PROTOBUF_SOURCES})
    
    # Add generated protobuf files
    if(PROTO_SOURCES)
        target_sources(schemaregistry PRIVATE ${PROTO_SOURCES})
    endif()
endif()

# Conditionally add Rules sources (includes CEL, encryption, jsonata)
if(SCHEMAREGISTRY_WITH_RULES)
    file(GLOB_RECURSE RULES_HEADERS "include/schemaregistry/rules/*.h")
    file(GLOB_RECURSE RULES_SOURCES "src/rules/*.cpp")
    file(GLOB_RECURSE RULES_INTERNAL_HEADERS "src/internal/schemaregistry/rules/*.h")
    target_sources(schemaregistry PRIVATE ${RULES_HEADERS} ${RULES_SOURCES} ${RULES_INTERNAL_HEADERS})
endif()

# Provide a namespaced ALIAS for consumers (and internal consistency)
# Build-tree alias matching the installed namespace
add_library(schemaregistry::schemaregistry ALIAS schemaregistry)

# Set library version metadata for shared library consumers/package managers
set_target_properties(schemaregistry PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# Setup formatting
option(SCHEMAREGISTRY_FORMAT_SOURCE "Enable code formatting" OFF)
if(SCHEMAREGISTRY_FORMAT_SOURCE)
    include(cmake/Format.cmake)
    Format(schemaregistry include)
    Format(schemaregistry src)
endif()

# Compiler features
target_compile_features(schemaregistry PUBLIC cxx_std_17)

# Compiler options
target_compile_options(schemaregistry
    PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
            -Wall -Wno-unused-variable -Wno-unused-lambda-capture>
)

# Include directories
target_include_directories(schemaregistry
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/internal>
        $<$<BOOL:${PROTO_INCLUDE_DIR}>:${PROTO_INCLUDE_DIR}>
)

# Link libraries conditionally based on enabled features
# Use BUILD_INTERFACE so install(EXPORT ...) does not reference non-exported
# in-tree dependency targets (avoids CMake install errors).

# Core libraries (always linked)
target_link_libraries(schemaregistry
    PUBLIC
        $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
    PRIVATE
        OpenSSL::Crypto
        cpr::cpr
        absl::strings # for base64_decode in JsonValue
)

# Conditionally link Avro
if(SCHEMAREGISTRY_WITH_AVRO)
    target_link_libraries(schemaregistry
        PUBLIC
            $<BUILD_INTERFACE:unofficial::avro-cpp::avrocpp>
    )
endif()

# Conditionally link JSON libraries  
if(SCHEMAREGISTRY_WITH_JSON)
    target_link_libraries(schemaregistry
        PRIVATE
            $<BUILD_INTERFACE:jsoncons>
    )
endif()

# Conditionally link Protobuf
if(SCHEMAREGISTRY_WITH_PROTOBUF)
    target_link_libraries(schemaregistry
        PUBLIC
            $<BUILD_INTERFACE:protobuf::libprotobuf>
    )
endif()

# Conditionally link Rules dependencies (CEL, encryption, KMS providers)
if(SCHEMAREGISTRY_WITH_RULES)
    target_link_libraries(schemaregistry
        PRIVATE
            # CEL (Common Expression Language) libraries
            $<BUILD_INTERFACE:cel_cpp>
            $<BUILD_INTERFACE:cel_cpp_parser>
            $<BUILD_INTERFACE:cel_cpp_minimal_descriptor_set>
            
            # JSON processing for rules
            $<BUILD_INTERFACE:jsonata>
            
            # Cryptography library
            $<BUILD_INTERFACE:tink::static>
            
            # AWS SDK for KMS driver
            aws-cpp-sdk-core
            aws-cpp-sdk-kms
            aws-cpp-sdk-sts
            aws-cpp-sdk-cognito-identity
            aws-cpp-sdk-identity-management
            
            # Azure SDK for Azure KMS driver
            Azure::azure-security-keyvault-keys
            Azure::azure-identity
            
            # Google Cloud SDK for GCP KMS driver
            google-cloud-cpp::kms

            # HashiCorp Vault SDK for Vault KMS driver
            libvault::vault
    )
endif()

# Add conditional compile definitions for C++ feature detection
if(SCHEMAREGISTRY_WITH_AVRO)
    target_compile_definitions(schemaregistry PUBLIC SCHEMAREGISTRY_USE_AVRO)
endif()

if(SCHEMAREGISTRY_WITH_JSON)
    target_compile_definitions(schemaregistry PUBLIC SCHEMAREGISTRY_USE_JSON)
endif()

if(SCHEMAREGISTRY_WITH_PROTOBUF)
    target_compile_definitions(schemaregistry PUBLIC SCHEMAREGISTRY_USE_PROTOBUF)
endif()

if(SCHEMAREGISTRY_WITH_RULES)
    target_compile_definitions(schemaregistry PUBLIC SCHEMAREGISTRY_USE_RULES)
endif()

option(SCHEMAREGISTRY_BUILD_TESTS "schemaregistry test suite" ON)
if(SCHEMAREGISTRY_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(test)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 1. Install the library itself (shared or static)
install(TARGETS schemaregistry
        EXPORT schemaregistryTargets  # This creates a target file for find_package()
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} # For static libraries (.a, .lib)
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} # For shared libraries (.so, .dylib)
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} # For module libraries or if you want shared libs in bin (less common for pure libs)
)

# 2. Install public header files
# Assuming your public headers are in the 'include' directory at the top level
install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
)

# Install generated Protobuf headers
if(PROTO_INCLUDE_DIR AND EXISTS ${PROTO_INCLUDE_DIR})
    install(DIRECTORY ${PROTO_INCLUDE_DIR}/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.h"
    )
endif()

# 3. Generate and install the CMake package configuration files
#    These files allow other projects to find your library using find_package(schemaregistry)

# Configure the package config file for package name 'schemaregistry'
configure_package_config_file(
        "cmake/schemaregistryConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/schemaregistryConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/schemaregistry"
)

# Generate the version file
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/schemaregistryConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY AnyNewerVersion
)

# Install the generated config and version files
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/schemaregistryConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/schemaregistryConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/schemaregistry"
)

# Install the export file for targets. This should be done AFTER all targets are defined.
# This file is used by the Config file to import your library targets.
install(EXPORT schemaregistryTargets
        FILE schemaregistryTargets.cmake
        NAMESPACE schemaregistry::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/schemaregistry"
)

# Add examples subdirectory (optional, can be disabled)
option(SCHEMAREGISTRY_BUILD_EXAMPLES "Build example programs" OFF)
if(SCHEMAREGISTRY_BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

