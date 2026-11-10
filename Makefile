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

HOST_FLAGS := -std=c99 -O2 -Wall -Wextra -Werror -pedantic -Iinclude
AMIGA_FLAGS := -std=gnu99 -O2 -Wall -Wextra -fomit-frame-pointer -noixemul
AMIGA_INCLUDES := -Iinclude -I$(SDK)/libnix/include -I$(SDK)/ndk-include -I$(SDK)/include
# DockAwesome targets the Vampire 68080 with AMMX and SAGA only.
CPU_FLAGS := -m68080
AMIGA_FLAGS += -DDOCK_AMMX
AMIGA_OUTPUT := build/DockAwesome
AMIGA_OBJDIR := build/obj080
AMIGA_ASM_OBJECTS := $(AMIGA_OBJDIR)/ammx.o
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
	"$(PYTHON)" tools/build-version.py "$(notdir $@)" "$(AMIGA_OBJDIR)/build_version.c" "$(RELEASE)"
	"$(AMIGA_CC)" $(AMIGA_INCLUDES) $(AMIGA_FLAGS) $(CPU_FLAGS) -c "$(AMIGA_OBJDIR)/build_version.c" -o "$(AMIGA_OBJDIR)/build_version.o"
	"$(AMIGA_CC)" $(CPU_FLAGS) -noixemul -Wl,-u,___stkinit $(AMIGA_OBJECTS) "$(AMIGA_OBJDIR)/build_version.o" -o "$@"

HOST_CORE := src/dock_core.c src/dock_compose.c src/dock_renderer.c src/dock_geometry.c src/dock_interaction.c
HOST_HEADERS := include/dock_core.h include/dock_compose.h include/dock_renderer.h include/dock_geometry.h include/dock_interaction.h src/dock_pixel.h

SANITIZE := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
HOST_TESTS := build/test_core build/test_interaction build/test_geometry build/test_prefs build/test_profile build/test_bob
HOST_SANITIZE_TESTS := $(HOST_TESTS:=_sanitize)

$(HOST_TESTS) build/bench_core: build/%: $(HOST_CORE) $(HOST_HEADERS) tests/%.c
	@mkdir -p build
	$(HOST_CC) $(HOST_FLAGS) $(filter %.c,$^) -o "$@"
$(HOST_SANITIZE_TESTS): build/%_sanitize: $(HOST_CORE) $(HOST_HEADERS) tests/%.c
	@mkdir -p build
	$(HOST_CC) $(HOST_FLAGS) $(SANITIZE) $(filter %.c,$^) -o "$@"
build/test_prefs build/test_prefs_sanitize: src/dock_prefs.c include/dock_prefs.h
build/test_profile build/test_profile_sanitize: src/dock_profile.c include/dock_profile.h
build/test_bob build/test_bob_sanitize: src/dock_bob.c include/dock_bob.h
build/test_core_be: $(HOST_CORE) $(HOST_HEADERS) tests/test_core.c
	@mkdir -p build
	$(HOST_CC) $(HOST_FLAGS) -DDOCK_EMULATE_BIG_ENDIAN $(filter %.c,$^) -o "$@"

host-test: build/test_core build/test_core_be build/test_interaction build/test_geometry build/test_prefs build/test_profile build/test_bob
	for t in $^; do ./$$t || exit 1; done
host-bench: build/bench_core
	./build/bench_core
host-sanitize: $(HOST_SANITIZE_TESTS)
	for t in $^; do ./$$t || exit 1; done

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

# Test release: stage DockAwesome with its icon and notes
# in build/release/DockAwesome. Pack that drawer with LhA on the Amiga.
release:
	@test -n "$(RELEASE)" || { echo 'Set RELEASE, for example: make release RELEASE="1.0 beta 1"' >&2; exit 1; }
	$(RM) build/DockAwesome
	$(MAKE) RELEASE="$(RELEASE)" amiga
	$(RM) -r build/release
	@mkdir -p build/release/DockAwesome
	cp build/DockAwesome build/DockAwesome.info build/release/DockAwesome/
	cp release/ReadMe.txt README.md RELEASE_NOTES.md build/release/DockAwesome/
	@echo "Staged build/release/DockAwesome for DockAwesome-$(RELEASE_FILE).lha"

clean:
	$(RM) -r build

-include $(AMIGA_C_OBJECTS:.o=.d)

PROBE_OBJECTS := $(addprefix $(AMIGA_OBJDIR)/,ammx_probe.o pip_probe.o pip_coexist_probe.o pip_align_probe.o)
PIP_PROBES := build/DockAwesomePipProbe080 build/DockAwesomePipCoexistProbe080 build/DockAwesomePipAlignProbe080

.PHONY: ammx-probe pip-probe pip-coexist-probe pip-align-probe
ammx-probe: build/DockAwesomeAMMXProbe
pip-probe: build/DockAwesomePipProbe080
pip-coexist-probe: build/DockAwesomePipCoexistProbe080
pip-align-probe: build/DockAwesomePipAlignProbe080

$(PROBE_OBJECTS): $(AMIGA_OBJDIR)/%.o: tools/%.c | check-toolchain
	@mkdir -p "$(@D)"
	"$(AMIGA_CC)" $(AMIGA_INCLUDES) $(AMIGA_FLAGS) $(CPU_FLAGS) -c "$<" -o "$@"
$(AMIGA_OBJDIR)/ammx_probe.o: include/dock_core.h
$(filter $(AMIGA_OBJDIR)/pip_%,$(PROBE_OBJECTS)): include/amiga_pip.h

build/DockAwesomeAMMXProbe: $(addprefix $(AMIGA_OBJDIR)/,dock_core.o dock_geometry.o dock_interaction.o ammx_probe.o ammx.o)
	"$(AMIGA_CC)" $(CPU_FLAGS) -noixemul -Wl,-u,___stkinit $^ -o "$@"

build/DockAwesomePipProbe080: $(AMIGA_OBJDIR)/pip_probe.o $(AMIGA_OBJDIR)/amiga_pip.o
build/DockAwesomePipCoexistProbe080: $(AMIGA_OBJDIR)/pip_coexist_probe.o $(AMIGA_OBJDIR)/amiga_pip.o
build/DockAwesomePipAlignProbe080: $(AMIGA_OBJDIR)/pip_align_probe.o $(AMIGA_OBJDIR)/amiga_pip.o
$(PIP_PROBES): Makefile
	"$(AMIGA_CC)" $(CPU_FLAGS) -noixemul -Wl,-u,___stkinit $(filter %.o,$^) -o "$@"
