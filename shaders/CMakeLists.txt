if(WIN32)
    add_definitions(-DNOMINMAX)
    set(GLSL_COMPILER glslangValidator.exe)
    set(GLSL_COMPILER_FLAGS -V -g)
else()
    set(GLSL_COMPILER glslangValidator)
    set(GLSL_COMPILER_FLAGS -V -g)
endif(WIN32)


file(GLOB_RECURSE GLSL_SOURCE_FILES CONFIGURE_DEPENDS "glsl/*")

message(STATUS "${PROJECT_SOURCE_DIR}")

foreach(GLSL ${GLSL_SOURCE_FILES})
    # Get the immediate parent directory name
    get_filename_component(PARENT_DIR ${GLSL} DIRECTORY)
    get_filename_component(PARENT_DIR_NAME ${PARENT_DIR} NAME)
    # Get filename components
    get_filename_component(FILE_NAME ${GLSL} NAME_WE)
    get_filename_component(FILE_EXT ${GLSL} LAST_EXT)
    string(REPLACE "." "" FILE_TYPE ${FILE_EXT})
    if(NOT ${FILE_TYPE} STREQUAL h AND NOT ${FILE_TYPE} STREQUAL spv)
        set(SPIRV "${PROJECT_SOURCE_DIR}/shaders/glsl/${PARENT_DIR_NAME}/${FILE_NAME}_${FILE_TYPE}.spv")
        add_custom_command( OUTPUT ${SPIRV}
                            COMMAND ${GLSL_COMPILER} ${GLSL} ${GLSL_COMPILER_FLAGS} -o ${SPIRV}
                            DEPENDS ${GLSL})
        list(APPEND SPIRV_SHADERS ${SPIRV})
    endif()
endforeach(GLSL)

add_custom_target(Shaders DEPENDS ${SPIRV_SHADERS})
