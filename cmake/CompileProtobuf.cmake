# Function to compile protobuf files in the proto/ directory
function(CompileProtobuf directory)
    # Parse arguments - second argument is optional extra_proto_path
    set(extra_proto_path "")
    if(ARGC GREATER 1)
        set(extra_proto_path --proto_path=${ARGV1})
    endif()
    
    # Find all .proto files in the proto/ directory only
    file(GLOB_RECURSE PROTO_FILES "${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto")

    if(PROTO_FILES)
        # Create directory for generated files
        set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/${directory}")
        file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

        foreach(PROTO_FILE ${PROTO_FILES})
            # Get the file name without extension
            get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)

            # Get relative path from proto/ directory for better organization
            file(RELATIVE_PATH PROTO_REL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/proto" "${PROTO_FILE}")
            get_filename_component(PROTO_REL_DIR ${PROTO_REL_PATH} DIRECTORY)

            # Generate the output file names in a directory structure that matches the source
            set(PROTO_OUT_DIR "${PROTO_GEN_DIR}/${PROTO_REL_DIR}")
            file(MAKE_DIRECTORY ${PROTO_OUT_DIR})
            set(PROTO_SRCS "${PROTO_OUT_DIR}/${PROTO_NAME}.pb.cc")
            set(PROTO_HDRS "${PROTO_OUT_DIR}/${PROTO_NAME}.pb.h")

            # Add custom command to generate C++ files from .proto
            add_custom_command(
                OUTPUT 
                    ${PROTO_SRCS} 
                    ${PROTO_HDRS}
                COMMAND 
                    ${Protobuf_PROTOC_EXECUTABLE}
                ARGS 
                    --cpp_out=${PROTO_GEN_DIR}
                    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
                    ${extra_proto_path}
                    ${PROTO_FILE}
                DEPENDS 
                    ${PROTO_FILE}
                COMMENT 
                    "Generating C++ code from proto/${PROTO_REL_PATH}"
                VERBATIM
            )

            # Add generated files to the list
            list(APPEND PROTO_GENERATED_SRCS ${PROTO_SRCS})
            list(APPEND PROTO_GENERATED_HDRS ${PROTO_HDRS})
        endforeach()

        # Set variables in parent scope
        set(PROTO_SOURCES ${PROTO_GENERATED_SRCS} PARENT_SCOPE)
        set(PROTO_HEADERS ${PROTO_GENERATED_HDRS} PARENT_SCOPE)
        set(PROTO_INCLUDE_DIR ${PROTO_GEN_DIR} PARENT_SCOPE)
    endif()
endfunction()