CC ?= cc
PKG_CONFIG ?= pkg-config
PYTHON ?= python3
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += -D_GNU_SOURCE -Isrc -Ibuild
PACKAGES = wayland-client cairo libcurl json-c
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -lm
WAYLAND_SCANNER ?= wayland-scanner
XDG_SHELL = $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
PREFIX ?= $(HOME)/.local
PROTOCOL = protocols/wlr-layer-shell-unstable-v1.xml

.PHONY: all install setup clean test
all: build/voxflow

build:
	mkdir -p $@

build/layer-shell.h: $(PROTOCOL) | build
	$(WAYLAND_SCANNER) client-header $< $@

build/layer-shell.c: $(PROTOCOL) | build
	$(WAYLAND_SCANNER) private-code $< $@

build/xdg-shell.c: $(XDG_SHELL) | build
	$(WAYLAND_SCANNER) private-code $< $@

build/voxflow: src/voxflow.c src/cloud.c src/http.c src/indicator.c build/layer-shell.c build/xdg-shell.c src/voxflow.h src/http.h build/layer-shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(filter %.c,$^) $(LDLIBS) -o $@

build/voxflow-test: src/voxflow.c src/cloud.c tests/fakes.c src/voxflow.h src/http.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(filter %.c,$^) $(LDLIBS) -o $@

build/test-recorder: tests/recorder.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $@

test: build/voxflow build/voxflow-test build/test-recorder
	$(PYTHON) tests/test_voxflow.py
	$(PYTHON) tests/test_setup.py

install: build/voxflow assets/voxflow.png
	install -Dm755 $< "$(DESTDIR)$(PREFIX)/bin/voxflow"
	install -Dm644 assets/voxflow.png "$(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/voxflow.png"

setup: install
	$(PYTHON) scripts/setup.py "$(PREFIX)" "$(DESTDIR)"

clean:
	rm -rf build
