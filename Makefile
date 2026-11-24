# Libreworks / Bonbon build helpers
#   make / make libreworks  — normal Libreworks Release build in build/
#   make bonbon             — Bonbon-branded build in build-bonbon/
#   make package            — .deb + AppImage + tar.gz for Libreworks
#   make bonbon-package     — same for Bonbon
#   make test               — ctest in build/
#   make clean              — remove build/ and build-bonbon/

BUILD ?= build
BONBON_BUILD ?= build-bonbon
CMAKE ?= cmake
NINJA ?= ninja
JOBS ?= $(shell nproc 2>/dev/null || echo 4)

.PHONY: all libreworks bonbon package bonbon-package test clean configure configure-bonbon

all: libreworks

configure:
	@mkdir -p $(BUILD)
	$(CMAKE) -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release -DLW_BRAND=Libreworks

libreworks: configure
	$(CMAKE) --build $(BUILD) -j$(JOBS)

configure-bonbon:
	@mkdir -p $(BONBON_BUILD)
	$(CMAKE) -S . -B $(BONBON_BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release -DLW_BRAND=Bonbon

bonbon: configure-bonbon
	$(CMAKE) --build $(BONBON_BUILD) -j$(JOBS)

package: libreworks
	cd $(BUILD) && cpack -G DEB
	packaging/build-appimage.sh $(BUILD)

bonbon-package: bonbon
	cd $(BONBON_BUILD) && cpack -G DEB
	packaging/build-appimage.sh $(BONBON_BUILD)

test: libreworks
	cd $(BUILD) && QT_QPA_PLATFORM=offscreen ctest --output-on-failure -j$(JOBS)

clean:
	rm -rf $(BUILD) $(BONBON_BUILD)
