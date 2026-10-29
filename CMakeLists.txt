cmake_minimum_required(VERSION 3.20)
project(RSGym C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()
find_package(ZLIB REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# rc-core: headless game engine (no render deps). See rc-core/README.md.
file(GLOB RC_CORE_SOURCES CONFIGURE_DEPENDS rc-core/*.c)
add_library(rc-core STATIC ${RC_CORE_SOURCES})
target_include_directories(rc-core PUBLIC rc-core)
target_link_libraries(rc-core PUBLIC ZLIB::ZLIB)

# rc-content: OSRS content hooks on top of rc-core, linked as objects so
# registration functions are never dropped by the static linker.
file(GLOB_RECURSE RC_CONTENT_SOURCES CONFIGURE_DEPENDS rc-content/*.c)
add_library(rc-content-objects OBJECT ${RC_CONTENT_SOURCES})
target_include_directories(rc-content-objects PUBLIC rc-content)
target_link_libraries(rc-content-objects PUBLIC rc-core)

add_library(rc-content-link INTERFACE)
add_dependencies(rc-content-link rc-content-objects)
target_include_directories(rc-content-link INTERFACE rc-content)
target_link_libraries(rc-content-link INTERFACE
    "$<TARGET_OBJECTS:rc-content-objects>" rc-core)

# rc-env: RL interface (observations, masked actions, tasks, replays) over a gym world.
file(GLOB RC_ENV_SOURCES CONFIGURE_DEPENDS rc-env/*.c)  # bench/ and puffer/ build separately
add_library(rc-env STATIC ${RC_ENV_SOURCES})
target_include_directories(rc-env PUBLIC rc-env)
target_link_libraries(rc-env PUBLIC rc-core rc-content-link m pthread)

# rc-viewer: raylib frontend, vendored prebuilt raylib in lib/raylib.
file(GLOB RC_VIEWER_SOURCES CONFIGURE_DEPENDS rc-viewer/*.c)
add_executable(rc-viewer ${RC_VIEWER_SOURCES})
target_include_directories(rc-viewer PRIVATE ${CMAKE_SOURCE_DIR}/lib/raylib/include)
target_link_directories(rc-viewer PRIVATE ${CMAKE_SOURCE_DIR}/lib/raylib/lib)
target_link_libraries(rc-viewer rc-env rc-core rc-content-link raylib m dl pthread)
if(UNIX AND NOT APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(rc-viewer OpenGL::GL)
endif()

add_test(NAME test_gymmap
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests -p "test_*.py")
set_tests_properties(test_gymmap PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Headless checks of the built gym data (skip until tools/build_mode.py has built the mode).
foreach(GYM_TEST test_gym_world test_gym_multiplayer test_gym_skilling test_gym_rules test_gym_bosses)
    add_executable(${GYM_TEST} tests/${GYM_TEST}.c)
    target_link_libraries(${GYM_TEST} rc-core rc-content-link m)
    add_test(NAME ${GYM_TEST} COMMAND ${GYM_TEST})
    set_tests_properties(${GYM_TEST} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} SKIP_RETURN_CODE 77)
endforeach()

add_executable(test_env tests/test_env.c)
target_link_libraries(test_env rc-env)
add_test(NAME test_env COMMAND test_env)
set_tests_properties(test_env PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} SKIP_RETURN_CODE 77
    ENVIRONMENT "RS_GYM_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(rs-gym-bench rc-env/bench/bench.c)
target_link_libraries(rs-gym-bench rc-env)
add_executable(rs-gym-rollout rc-env/tools/rollout.c)
target_link_libraries(rs-gym-rollout rc-env)

# PufferLib 5.0 binding: rs_gym_puffer bundles rc-env with puf_* for a PufferLib checkout
# (tools/puffer_overlay.sh); the test builds it against a stand-in pufferenv.h.
add_executable(test_puffer_binding tests/test_puffer_binding.c rc-env/puffer/binding.c)
target_include_directories(test_puffer_binding PRIVATE tests/puffer_stub rc-env/puffer)
target_link_libraries(test_puffer_binding rc-env)
add_test(NAME test_puffer_binding COMMAND test_puffer_binding)
set_tests_properties(test_puffer_binding PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} SKIP_RETURN_CODE 77)

# Built by tools/puffer_overlay.sh: one position-independent archive for a PufferLib
# checkout's trainer, compiled against its pufferenv.h.
set(RS_GYM_PUFFERLIB "" CACHE PATH "PufferLib 5.0 checkout to build the rs_gym binding for")
set(RS_GYM_RAYLIB_INCLUDE "" CACHE PATH "raylib include directory of that checkout")
if(RS_GYM_PUFFERLIB)
    add_library(rs_gym_puffer STATIC ${RC_CORE_SOURCES} ${RC_CONTENT_SOURCES} ${RC_ENV_SOURCES}
        rc-env/puffer/binding.c)
    set_target_properties(rs_gym_puffer PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(rs_gym_puffer PRIVATE rc-core rc-content rc-env rc-env/puffer
        ${RS_GYM_PUFFERLIB}/src ${RS_GYM_PUFFERLIB}/vendor ${RS_GYM_RAYLIB_INCLUDE})
endif()
