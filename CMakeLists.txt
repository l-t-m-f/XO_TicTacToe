cmake_minimum_required(VERSION 3.24)
project(TIC_TAC_TOE C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_COMPILER GCC)

# Check for OpenMP support
find_package(OpenMP REQUIRED)

# Add OpenMP flags to the compiler
if(OPENMP_FOUND)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    endif()
endif()

file(COPY "gfx" DESTINATION "${CMAKE_BINARY_DIR}")
file(COPY "font" DESTINATION "${CMAKE_BINARY_DIR}")
file(COPY "clip" DESTINATION "${CMAKE_BINARY_DIR}")

set(CMAKE_C_FLAGS_DEBUG_ESSENTIALS "-pedantic -Wall -Wextra -g3 -Wfloat-equal -Wundef -Wshadow -Wpointer-arith -Wwrite-strings -Waggregate-return -Wunreachable-code")
set(CMAKE_C_FLAGS_DEBUG_STRICT "-Wstrict-prototypes -Wstrict-overflow=2")
set(CMAKE_C_FLAGS_DEBUG_CAST "-Wcast-align -Wcast-qual -Wconversion")
set(CMAKE_C_FLAGS_DEBUG_SWITCH "-Wswitch-default -Wswitch-enum")

add_executable(${PROJECT_NAME} "game.c")

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} ${CMAKE_C_FLAGS_DEBUG_ESSENTIALS} ${CMAKE_C_FLAGS_DEBUG_SWITCH} ${CMAKE_C_FLAGS_DEBUG_STRICT} ${CMAKE_C_FLAGS_DEBUG_CAST} ${CMAKE_C_FLAGS_EXTRA}" CACHE STRING "C Flags" FORCE)

target_link_libraries(${PROJECT_NAME} ${OpenMP_C_LIBRARIES} -lmingw32 -lSDL2main -lSDL2 -lSDL2_image -lSDL2_ttf -lSDL2_mixer)