.PHONY: all install clean

# The arch suffix is CMake's job, not this file's — see the block at the end of
# CMakeLists.txt. Doing it here with `uname -m` would name a cross-compiled
# binary after the build host. This only has to find what CMake produced.
BUILD_DIR := build
BINARY     = $(notdir $(wildcard $(BUILD_DIR)/ubersdr-clock_*))
INSTALL_DIR := /opt/ubersdr-clock

all:
	cmake -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
	cmake --build $(BUILD_DIR) -j$(shell nproc)
	@cp $(BUILD_DIR)/ubersdr-clock_* .
	@echo "Built $$(ls ubersdr-clock_*)"

install: all
	cmake --install $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) ubersdr-clock_*
