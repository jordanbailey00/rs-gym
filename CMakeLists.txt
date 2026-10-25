cmake_minimum_required(VERSION 3.20)
project(RSGym C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()
find_package(ZLIB REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# rc-core: headless game engine (no render deps). See rc-core/README.md.
file(GLOB RC_CORE_SOURCES rc-core/*.c)
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

# rc-viewer: raylib frontend, vendored prebuilt raylib in lib/raylib.
file(GLOB RC_VIEWER_SOURCES CONFIGURE_DEPENDS rc-viewer/*.c)
add_executable(rc-viewer ${RC_VIEWER_SOURCES})
target_include_directories(rc-viewer PRIVATE ${CMAKE_SOURCE_DIR}/lib/raylib/include)
target_link_directories(rc-viewer PRIVATE ${CMAKE_SOURCE_DIR}/lib/raylib/lib)
target_link_libraries(rc-viewer rc-core rc-content-link raylib m dl pthread)
if(UNIX AND NOT APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(rc-viewer OpenGL::GL)
endif()

add_test(NAME test_gymmap
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests -p "test_*.py")
set_tests_properties(test_gymmap PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Headless check of the built gym data (skips until tools/build_gym.py has run).
add_executable(test_gym_world tests/test_gym_world.c)
target_link_libraries(test_gym_world rc-core rc-content-link m)
add_test(NAME test_gym_world COMMAND test_gym_world)
set_tests_properties(test_gym_world PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} SKIP_RETURN_CODE 77)
