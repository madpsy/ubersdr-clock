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
    src/Dcf77Decoder.cpp
    src/MsfDecoder.cpp
    src/AllouisDecoder.cpp
    src/TimeFrameVoter.cpp
)

target_include_directories(ubersdr-clock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Offline decoder accuracy test.
#
# Synthesises WWV/WWVH/WWVB at several sample rates, sub-sample phases and
# noise levels -- including leap-second minutes and both station tags -- and
# checks the decoded timestamps and the edge error against the truth it
# generated. Needs no receiver, no network and no recording, so it can be run
# on any change to the decoders.
#
# This is the evidence behind the timing constants. kNominalDelaySamples and
# the voter's calibration figures were measured against real off-air audio
# through a real receive chain, which this cannot reproduce -- it reports the
# synthetic-exact value alongside so the difference stays visible -- so treat a
# change to them as needing off-air evidence, not a passing run here.
#
# Not installed: it is a development tool.
# ---------------------------------------------------------------------------
add_executable(ubersdr-clock-decodertest
    tools/decodertest.cpp
    src/WwvDecoder.cpp
    src/WwvbDecoder.cpp
    src/TimeFrameVoter.cpp
)
target_include_directories(ubersdr-clock-decodertest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ubersdr-clock-decodertest PRIVATE -Wall -Wextra)
find_package(Threads REQUIRED)
target_link_libraries(ubersdr-clock-decodertest PRIVATE Threads::Threads)

# The DCF77 decoder's own test: AM and PM synthesised per PTB across noise,
# carrier offset, interference, fades, a CET->CEST change and a leap second,
# then five minutes of real DCF77 IQ (tools/testdata/dcf77_live.wav) decoded
# and checked against the time its recording shows. A path on the command line
# replaces the recording.
add_executable(ubersdr-clock-dcf77test
    tools/dcf77test.cpp
    src/Dcf77Decoder.cpp
    src/TimeFrameVoter.cpp
)
target_include_directories(ubersdr-clock-dcf77test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ubersdr-clock-dcf77test PRIVATE -Wall -Wextra)
target_compile_definitions(ubersdr-clock-dcf77test PRIVATE
    DCF77_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/tools/testdata/dcf77_live.wav")

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

# MSF's and Allouis's own tests, as ubersdr-ntp has them: synthesised signals
# (noise, sample-grid offsets, carrier offset, the summer-time change, a leap
# second for MSF, a maintenance outage for Allouis), then a recording from
# M9PSY-1 with radiod's GPS capture stamps beside it (tools/testdata), checked
# second by second against those stamps. A path on the command line replaces
# the recording.
add_executable(ubersdr-clock-msftest
    tools/msftest.cpp
    src/MsfDecoder.cpp
    src/TimeFrameVoter.cpp
)
target_include_directories(ubersdr-clock-msftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ubersdr-clock-msftest PRIVATE -Wall -Wextra)
target_compile_definitions(ubersdr-clock-msftest PRIVATE
    MSF_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/tools/testdata/msf_m9psy1_20260925T0120Z.wav")

add_executable(ubersdr-clock-allouistest
    tools/allouistest.cpp
    src/AllouisDecoder.cpp
    src/TimeFrameVoter.cpp
)
target_include_directories(ubersdr-clock-allouistest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ubersdr-clock-allouistest PRIVATE -Wall -Wextra)
target_compile_definitions(ubersdr-clock-allouistest PRIVATE
    ALS162_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/tools/testdata/als162_m9psy1_20260925T0120Z.wav")
