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
list(FILTER CPU_SOURCES EXCLUDE REGEX "src/main\.cpp$")
list(FILTER CPU_SOURCES EXCLUDE REGEX "src/net/test_.*\.cpp$")
list(FILTER CPU_SOURCES EXCLUDE REGEX "src/net/rts/example_usage\.cpp$")
list(FILTER CPU_SOURCES EXCLUDE REGEX "src/net/voxel/.*\.cpp$")
list(FILTER CPU_SOURCES EXCLUDE REGEX "src/main_sdfcraft\.cpp$")
list(FILTER CPU_SOURCES EXCLUDE REGEX "src/sdfcraft/.*\.cpp$")

# GPU target sources (exclude main.cpp, keep main_gpu.cpp)
set(GPU_SOURCES ${ALL_SOURCES})
list(FILTER GPU_SOURCES EXCLUDE REGEX "src/main\.cpp$")
list(FILTER GPU_SOURCES EXCLUDE REGEX "src/main_net\.cpp$")
list(FILTER GPU_SOURCES EXCLUDE REGEX "src/net/test_.*\.cpp$")
list(FILTER GPU_SOURCES EXCLUDE REGEX "src/net/rts/example_usage\.cpp$")
list(FILTER GPU_SOURCES EXCLUDE REGEX "src/net/voxel/.*\.cpp$")
list(FILTER GPU_SOURCES EXCLUDE REGEX "src/main_sdfcraft\.cpp$")
list(FILTER GPU_SOURCES EXCLUDE REGEX "src/sdfcraft/.*\.cpp$")

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

# === Network Test (rollback + fixed-point) ===
add_executable(test_net src/net/test_rollback.cpp src/net/core/rollback_engine.cpp)
target_include_directories(test_net PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(test_net PRIVATE ws2_32)
endif()

# === Voxel Network Test ===
add_executable(test_voxel src/net/test_voxel.cpp)
target_include_directories(test_voxel PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_voxel PRIVATE glm::glm)
if(WIN32)
    target_link_libraries(test_voxel PRIVATE ws2_32)
endif()

# === Zlib for compression ===
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zlib)

# === Link zlib into main network-enabled targets ===
target_include_directories(${PROJECT_NAME} PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
target_link_libraries(${PROJECT_NAME} PRIVATE zlibstatic)
target_include_directories(MassRTS_GPU PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
target_link_libraries(MassRTS_GPU PRIVATE zlibstatic)

# === Compression Test ===
add_executable(test_compression src/net/test_compression.cpp)
target_include_directories(test_compression PRIVATE ${CMAKE_SOURCE_DIR}/src ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
target_link_libraries(test_compression PRIVATE zlibstatic)
if(WIN32)
    target_link_libraries(test_compression PRIVATE ws2_32)
endif()

# === TCP FileTransfer Test ===
add_executable(test_tcp src/net/test_tcp.cpp)
target_include_directories(test_tcp PRIVATE ${CMAKE_SOURCE_DIR}/src ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
target_link_libraries(test_tcp PRIVATE zlibstatic)
if(WIN32)
    target_link_libraries(test_tcp PRIVATE ws2_32)
endif()

# === Persistence Test ===
add_executable(test_persistence src/net/test_persistence.cpp)
target_include_directories(test_persistence PRIVATE ${CMAKE_SOURCE_DIR}/src ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
target_link_libraries(test_persistence PRIVATE zlibstatic)
if(WIN32)
    target_link_libraries(test_persistence PRIVATE ws2_32)
endif()

# === SDFCraft (survival/build voxel mode, isolated from RTS) ===
add_executable(SDFCraft src/main_sdfcraft.cpp)
target_include_directories(SDFCraft PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${glad_SOURCE_DIR}/include
)
target_link_libraries(SDFCraft PRIVATE
    OpenGL::GL
    glfw
    glad
    glm::glm
)
if(WIN32)
    target_link_libraries(SDFCraft PRIVATE ws2_32)
endif()
add_custom_command(TARGET SDFCraft POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/shaders
    $<TARGET_FILE_DIR:SDFCraft>/shaders
)

# === PlanetViewer (Phase P1: fly around a real-scale 6371km planet) ===
add_executable(PlanetViewer src/main_planet.cpp)
target_include_directories(PlanetViewer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${glad_SOURCE_DIR}/include
)
target_link_libraries(PlanetViewer PRIVATE OpenGL::GL glfw glad glm::glm)
if(WIN32)
    target_link_libraries(PlanetViewer PRIVATE ws2_32)
endif()


# === SDFCraft headless logic test (no GL) ===
add_executable(test_sdfcraft src/sdfcraft/selftest.cpp)
target_include_directories(test_sdfcraft PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_sdfcraft PRIVATE glm::glm)
