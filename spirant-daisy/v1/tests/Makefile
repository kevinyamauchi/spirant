# Host tests for the System Bus comm layer.
#
# These build with the system compiler and run on the development machine --
# no embedded toolchain, no libDaisy, and deliberately no test framework. They
# are NOT part of the firmware build (../Makefile); run them by hand:
#
#   make -C spirant-daisy/v1/tests run
#
# The headers under ../comm are host-compilable on purpose: crc8.h and frame.h
# are a cross-language contract with the Pico's Rust serializer, and a contract
# that can only be exercised on hardware is a contract that goes untested.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1 -g

TESTS = crc8_test frame_test

.PHONY: all run clean
all: $(TESTS)

# -UNDEBUG: these tests are built entirely out of assert(), so a stray -DNDEBUG
# in the environment would turn every one of them into a no-op that "passes".
%_test: %_test.cpp ../comm/crc8.h ../comm/frame.h ../comm/param_table.h
	$(CXX) $(CXXFLAGS) -UNDEBUG -o $@ $<

run: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; echo; done
	@echo "all comm host tests passed"

clean:
	rm -f $(TESTS)
	rm -rf $(TESTS:=.dSYM)
