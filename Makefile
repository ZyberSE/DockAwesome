SHELL := /bin/sh

APOLLO_ROOT ?= $(HOME)/ApolloCrossDevLocal
TOOLCHAIN ?= $(APOLLO_ROOT)/Compilers/GCC-6.50-Stable
SDK := $(TOOLCHAIN)/m68k-amigaos
AMIGA_CC ?= $(TOOLCHAIN)/bin/m68k-amigaos-gcc
AMIGA_AS ?= $(TOOLCHAIN)/bin/vasmm68k_mot
AMIGA_OBJDUMP ?= $(TOOLCHAIN)/bin/m68k-amigaos-objdump
HOST_CC ?= cc
PYTHON ?= python3
ACP ?= acp
# A release name such as "1.0 beta 1" replaces the local build counter.
RELEASE ?=
empty :=
space := $(empty) $(empty)
RELEASE_FILE = $(subst $(space),,$(subst beta,b,$(RELEASE)))
AMMX ?= 0

HOST_FLAGS := -std=c99 -O2 -Wall -Wextra -Werror -pedantic -Iinclude
AMIGA_FLAGS := -std=gnu99 -O2 -Wall -Wextra -fomit-frame-pointer -noixemul
AMIGA_INCLUDES := -Iinclude -I$(SDK)/libnix/include -I$(SDK)/ndk-include -I$(SDK)/include
ifeq ($(AMMX),1)
CPU_FLAGS := -m68080
AMIGA_FLAGS += -DDOCK_AMMX
AMIGA_OUTPUT := build/DockAwesome080
AMIGA_OBJDIR := build/obj080
AMIGA_ASM_OBJECTS := $(AMIGA_OBJDIR)/ammx.o
else
CPU_FLAGS := -m68020
AMIGA_OUTPUT := build/DockAwesome
AMIGA_OBJDIR := build/obj020
AMIGA_ASM_OBJECTS :=
endif
AMIGA_C_SOURCES := $(sort $(wildcard src/*.c))
AMIGA_C_OBJECTS := $(patsubst src/%.c,$(AMIGA_OBJDIR)/%.o,$(AMIGA_C_SOURCES))
AMIGA_OBJECTS := $(AMIGA_C_OBJECTS) $(AMIGA_OBJDIR)/amiga_input.o $(AMIGA_ASM_OBJECTS)

.DEFAULT_GOAL := all
.PHONY: all amiga host-test host-bench host-sanitize check-toolchain verify package deploy release clean
all: amiga
amiga: $(AMIGA_OUTPUT) $(AMIGA_OUTPUT).info

$(AMIGA_OUTPUT).info: assets/DockAwesome-48.info
	@mkdir -p "$(@D)"
	cp "$<" "$@"

check-toolchain:
	@test -x "$(AMIGA_CC)" || { echo "Missing Amiga GCC: $(AMIGA_CC)" >&2; exit 1; }
	@test -x "$(AMIGA_AS)" || { echo "Missing vasm: $(AMIGA_AS)" >&2; exit 1; }

$(AMIGA_OBJDIR)/%.o: src/%.c include/dock_core.h Makefile | check-toolchain
	@mkdir -p "$(@D)"
	"$(AMIGA_CC)" $(AMIGA_INCLUDES) $(AMIGA_FLAGS) $(CPU_FLAGS) -MMD -MP -c "$<" -o "$@"

$(AMIGA_OBJDIR)/amiga_input.o: src/amiga_input.s Makefile | check-toolchain
	@mkdir -p "$(@D)"
	"$(AMIGA_AS)" -Fhunk -m68020 -opt-speed -o "$@" "$<"

$(AMIGA_OBJDIR)/ammx.o: src/ammx.s Makefile | check-toolchain
	@mkdir -p "$(@D)"
	"$(AMIGA_AS)" -Fhunk -m68080 -opt-speed -o "$@" "$<"

$(AMIGA_OUTPUT): $(AMIGA_OBJECTS) tools/build-version.py Makefile
	"$(PYTHON)" tools/build-version.py "$(if $(RELEASE),DockAwesome,$(notdir $@))" "$(AMIGA_OBJDIR)/build_version.c" "$(RELEASE)"
	"$(AMIGA_CC)" $(AMIGA_INCLUDES) $(AMIGA_FLAGS) $(CPU_FLAGS) -c "$(AMIGA_OBJDIR)/build_version.c" -o "$(AMIGA_OBJDIR)/build_version.o"
	"$(AMIGA_CC)" $(CPU_FLAGS) -noixemul -Wl,-u,___stkinit $(AMIGA_OBJECTS) "$(AMIGA_OBJDIR)/build_version.o" -o "$@"

HOST_CORE := src/dock_core.c src/dock_renderer.c src/dock_geometry.c src/dock_interaction.c
HOST_HEADERS := include/dock_core.h include/dock_renderer.h include/dock_geometry.h include/dock_interaction.h

build/test_core: $(HOST_CORE) $(HOST_HEADERS) tests/test_core.c
	@mkdir -p build
	$(HOST_CC) $(HOST_FLAGS) $(HOST_CORE) tests/test_core.c -o "$@"
build/test_interaction: $(HOST_CORE) $(HOST_HEADERS) tests/test_interaction.c
	@mkdir -p build
	$(HOST_CC) $(HOST_FLAGS) $(HOST_CORE) tests/test_interaction.c -o "$@"
build/test_geometry: $(HOST_CORE) $(HOST_HEADERS) tests/test_geometry.c
	@mkdir -p build
	$(HOST_CC) $(HOST_FLAGS) $(HOST_CORE) tests/test_geometry.c -o "$@"
build/test_prefs: $(HOST_CORE) $(HOST_HEADERS) src/dock_prefs.c include/dock_prefs.h tests/test_prefs.c
	@mkdir -p build
	$(HOST_CC) $(HOST_FLAGS) $(HOST_CORE) src/dock_prefs.c tests/test_prefs.c -o "$@"
build/test_core_be: $(HOST_CORE) $(HOST_HEADERS) tests/test_core.c
	@mkdir -p build
	$(HOST_CC) $(HOST_FLAGS) -DDOCK_EMULATE_BIG_ENDIAN $(HOST_CORE) tests/test_core.c -o "$@"
host-test: build/test_core build/test_core_be build/test_interaction build/test_geometry build/test_prefs
	./build/test_core
	./build/test_core_be
	./build/test_interaction
	./build/test_geometry
	./build/test_prefs

build/bench_core: $(HOST_CORE) $(HOST_HEADERS) tests/bench_core.c
	@mkdir -p build
	$(HOST_CC) $(HOST_FLAGS) $(HOST_CORE) tests/bench_core.c -o "$@"
host-bench: build/bench_core
	./build/bench_core

build/test_core_sanitize: $(HOST_CORE) $(HOST_HEADERS) tests/test_core.c
	@mkdir -p build
	$(HOST_CC) $(HOST_FLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(HOST_CORE) tests/test_core.c -o "$@"
build/test_interaction_sanitize: $(HOST_CORE) $(HOST_HEADERS) tests/test_interaction.c
	@mkdir -p build
	$(HOST_CC) $(HOST_FLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(HOST_CORE) tests/test_interaction.c -o "$@"
build/test_geometry_sanitize: $(HOST_CORE) $(HOST_HEADERS) tests/test_geometry.c
	@mkdir -p build
	$(HOST_CC) $(HOST_FLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(HOST_CORE) tests/test_geometry.c -o "$@"
build/test_prefs_sanitize: $(HOST_CORE) $(HOST_HEADERS) src/dock_prefs.c include/dock_prefs.h tests/test_prefs.c
	@mkdir -p build
	$(HOST_CC) $(HOST_FLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(HOST_CORE) src/dock_prefs.c tests/test_prefs.c -o "$@"
host-sanitize: build/test_core_sanitize build/test_interaction_sanitize build/test_geometry_sanitize build/test_prefs_sanitize
	./build/test_core_sanitize
	./build/test_interaction_sanitize
	./build/test_geometry_sanitize
	./build/test_prefs_sanitize

verify: amiga
	file "$(AMIGA_OUTPUT)"
	"$(AMIGA_OBJDUMP)" -f "$(AMIGA_OUTPUT)"

package: amiga
	@mkdir -p build/dist/DockAwesome
	cp "$(AMIGA_OUTPUT)" "$(AMIGA_OUTPUT).info" build/dist/DockAwesome/
	cp README.md build/dist/DockAwesome/
	cp -R docs build/dist/DockAwesome/

deploy: amiga
	@test -n "$(AMIGA_DEST)" || { echo "Set AMIGA_DEST to host:volume:." >&2; exit 1; }
	"$(ACP)" "$(AMIGA_OUTPUT).info" "$(AMIGA_OUTPUT)" "$(AMIGA_DEST)"

# Test release: stage the 68080 build as DockAwesome with its icon and notes
# in build/release/DockAwesome. Pack that drawer with LhA on the Amiga.
release:
	@test -n "$(RELEASE)" || { echo 'Set RELEASE, for example: make release RELEASE="1.0 beta 1"' >&2; exit 1; }
	$(RM) build/DockAwesome080
	$(MAKE) AMMX=1 RELEASE="$(RELEASE)" amiga
	$(RM) -r build/release
	@mkdir -p build/release/DockAwesome
	cp build/DockAwesome080 build/release/DockAwesome/DockAwesome
	cp build/DockAwesome080.info build/release/DockAwesome/DockAwesome.info
	cp release/ReadMe.txt README.md build/release/DockAwesome/
	@echo "Staged build/release/DockAwesome for DockAwesome-$(RELEASE_FILE).lha"

clean:
	$(RM) -r build

-include $(AMIGA_C_OBJECTS:.o=.d)

.PHONY: ammx-probe
ammx-probe:
	$(MAKE) AMMX=1 build/DockAwesomeAMMXProbe

build/obj080/ammx_probe.o: tools/ammx_probe.c include/dock_core.h | check-toolchain
	@mkdir -p "$(@D)"
	"$(AMIGA_CC)" $(AMIGA_INCLUDES) $(AMIGA_FLAGS) -DDOCK_AMMX -m68080 -c "$<" -o "$@"

build/DockAwesomeAMMXProbe: build/obj080/dock_core.o build/obj080/dock_geometry.o build/obj080/dock_interaction.o build/obj080/ammx_probe.o build/obj080/ammx.o
	"$(AMIGA_CC)" -m68080 -noixemul -Wl,-u,___stkinit $^ -o "$@"

.PHONY: pip-probe
pip-probe:
	$(MAKE) AMMX=1 build/DockAwesomePipProbe080

build/obj080/pip_probe.o: tools/pip_probe.c include/amiga_pip.h | check-toolchain
	@mkdir -p "$(@D)"
	"$(AMIGA_CC)" $(AMIGA_INCLUDES) $(AMIGA_FLAGS) -DDOCK_AMMX -m68080 -c "$<" -o "$@"

build/DockAwesomePipProbe080: build/obj080/pip_probe.o build/obj080/amiga_pip.o Makefile
	"$(AMIGA_CC)" -m68080 -noixemul -Wl,-u,___stkinit build/obj080/pip_probe.o build/obj080/amiga_pip.o -o "$@"

.PHONY: pip-coexist-probe
pip-coexist-probe:
	$(MAKE) AMMX=1 build/DockAwesomePipCoexistProbe080

build/obj080/pip_coexist_probe.o: tools/pip_coexist_probe.c include/amiga_pip.h | check-toolchain
	@mkdir -p "$(@D)"
	"$(AMIGA_CC)" $(AMIGA_INCLUDES) $(AMIGA_FLAGS) -DDOCK_AMMX -m68080 -c "$<" -o "$@"

build/DockAwesomePipCoexistProbe080: build/obj080/pip_coexist_probe.o build/obj080/amiga_pip.o Makefile
	"$(AMIGA_CC)" -m68080 -noixemul -Wl,-u,___stkinit build/obj080/pip_coexist_probe.o build/obj080/amiga_pip.o -o "$@"

.PHONY: pip-align-probe
pip-align-probe:
	$(MAKE) AMMX=1 build/DockAwesomePipAlignProbe080

build/obj080/pip_align_probe.o: tools/pip_align_probe.c include/amiga_pip.h | check-toolchain
	@mkdir -p "$(@D)"
	"$(AMIGA_CC)" $(AMIGA_INCLUDES) $(AMIGA_FLAGS) -DDOCK_AMMX -m68080 -c "$<" -o "$@"

build/DockAwesomePipAlignProbe080: build/obj080/pip_align_probe.o build/obj080/amiga_pip.o Makefile
	"$(AMIGA_CC)" -m68080 -noixemul -Wl,-u,___stkinit build/obj080/pip_align_probe.o build/obj080/amiga_pip.o -o "$@"
