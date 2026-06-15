cmake_minimum_required(VERSION 3.20)
project(MassRTS VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(MASS_RTS_UNIT_COUNT "Default unit count" 100000)
add_compile_definitions(DEFAULT_UNIT_COUNT=100000)

# Find OpenGL
find_package(OpenGL REQUIRED)

# GLFW
include(FetchContent)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.3.9
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# GLAD
FetchContent_Declare(
    glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG v0.1.36
)
FetchContent_MakeAvailable(glad)

# GLM
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
)
FetchContent_MakeAvailable(glm)

# Source files (exclude alternate main files from each target)
file(GLOB_RECURSE ALL_SOURCES "src/*.cpp" "src/*.c")

# CPU target sources (exclude main_gpu.cpp)
set(CPU_SOURCES ${ALL_SOURCES})
list(FILTER CPU_SOURCES EXCLUDE REGEX "main_gpu\.cpp$")

# GPU target sources (exclude main.cpp, keep main_gpu.cpp)
set(GPU_SOURCES ${ALL_SOURCES})
list(FILTER GPU_SOURCES EXCLUDE REGEX "src/main\.cpp$")

# === CPU Target (original, stable) ===
add_executable(${PROJECT_NAME} ${CPU_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${glad_SOURCE_DIR}/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    OpenGL::GL
    glfw
    glad
    glm::glm
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/shaders
    $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders
)

# === GPU Target (experimental GPU combat+movement) ===
add_executable(MassRTS_GPU ${GPU_SOURCES})

target_include_directories(MassRTS_GPU PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${glad_SOURCE_DIR}/include
)

target_link_libraries(MassRTS_GPU PRIVATE
    OpenGL::GL
    glfw
    glad
    glm::glm
)

if(WIN32)
    target_link_libraries(MassRTS_GPU PRIVATE ws2_32)
endif()

add_custom_command(TARGET MassRTS_GPU POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/shaders
    $<TARGET_FILE_DIR:MassRTS_GPU>/shaders
)
