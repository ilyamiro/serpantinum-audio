CXX ?= c++
PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra
VERSION := $(shell cat VERSION)
PW_CFLAGS = $(shell $(PKG_CONFIG) --cflags libpipewire-0.3)
PW_LIBS = $(shell $(PKG_CONFIG) --libs libpipewire-0.3)
OUT ?= serpantinum-audio
PREFIX ?= /usr/local
DESTDIR ?=

SOURCES := src/main.cpp $(wildcard src/*.h src/dsp/*.h)
TESTS := dsp_test dynamic_eq_test control_test

all: $(OUT)

$(OUT): $(SOURCES) VERSION
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -DSA_VERSION='"$(VERSION)"' $(PW_CFLAGS) -o $@ src/main.cpp $(LDFLAGS) $(PW_LIBS)

test: $(SOURCES)
	@set -e; for t in $(TESTS); do \
		$(CXX) $(CXXFLAGS) -UNDEBUG -o $$t.bin test/$$t.cpp; \
		./$$t.bin; \
		rm -f $$t.bin; \
	done

eval: test/eval.cpp $(SOURCES)
	$(CXX) $(CXXFLAGS) -o eval test/eval.cpp

install: $(OUT)
	install -Dm755 $(OUT) $(DESTDIR)$(PREFIX)/bin/serpantinum-audio
	install -Dm644 LICENSE $(DESTDIR)$(PREFIX)/share/licenses/serpantinum-audio/LICENSE

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/serpantinum-audio
	rm -rf $(DESTDIR)$(PREFIX)/share/licenses/serpantinum-audio

clean:
	rm -f $(OUT) eval $(addsuffix .bin,$(TESTS))

.PHONY: all test eval install uninstall clean
