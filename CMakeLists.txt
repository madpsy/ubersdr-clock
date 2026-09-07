cmake_minimum_required(VERSION 3.16)
project(ubersdr-clock LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# No Qt, no FFTW, no external libraries at all — the decoders are biquads, a
# running rotation and one correlation per second, and main.cpp is stdio plus
# integer calendar arithmetic. libstdc++ is the whole dependency list.
add_executable(ubersdr-clock
    src/main.cpp
    src/WwvDecoder.cpp
    src/WwvbDecoder.cpp
    src/TimeFrameVoter.cpp
)

target_include_directories(ubersdr-clock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
    target_compile_options(ubersdr-clock PRIVATE /W4)
else()
    target_compile_options(ubersdr-clock PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Architecture-suffixed output name
#
# Builds for different architectures can then live side by side in
# /opt/ubersdr-clock/. The suffixes deliberately match Go's GOARCH values so
# the audio extension can resolve the right binary with runtime.GOARCH, and so
# the name matches Docker's ${TARGETARCH} without translation.
#
# Keyed on CMAKE_SYSTEM_PROCESSOR rather than `uname -m`: they agree for a
# native build, but under a cross-toolchain uname reports the HOST, which would
# name an arm64 binary amd64 and hand the container the wrong one.
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(UBERSDR_ARCH "amd64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(UBERSDR_ARCH "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv6l|armv7l|arm)$")
    set(UBERSDR_ARCH "arm")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i386|i486|i586|i686|x86)$")
    set(UBERSDR_ARCH "386")
else()
    # Unknown target: fall back to whatever CMake detected rather than guess.
    set(UBERSDR_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    message(WARNING "Unrecognised CMAKE_SYSTEM_PROCESSOR '${CMAKE_SYSTEM_PROCESSOR}'; "
                    "binary will be named ubersdr-clock_${UBERSDR_ARCH}")
endif()

set_target_properties(ubersdr-clock PROPERTIES
    OUTPUT_NAME "ubersdr-clock_${UBERSDR_ARCH}"
)

message(STATUS "Binary will be built as ubersdr-clock_${UBERSDR_ARCH}")

install(TARGETS ubersdr-clock DESTINATION /opt/ubersdr-clock)
