# Host tests for the ESP32 parameter model.
#
# These build with the system compiler and run on the development machine --
# no Arduino toolchain, no ESP32 core. They are not part of the firmware build;
# run them by hand:
#
#   make -C spirant-hw-interface-esp32/tests run
#
# src/params/ is deliberately free of Arduino.h so this works. The parameter
# table is a cross-language contract with spirant-parameter-values-rs and with
# spirant-daisy/v1/comm/param_table.h, and a contract that can only be
# exercised on hardware is a contract that goes untested.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1 -g

SRC_DIR = ../spirant_hw_interface/src

TESTS = parameter_values_test usb_midi_parser_test monitor_state_test

.PHONY: all run clean
all: $(TESTS)

# -UNDEBUG: these tests are built entirely out of assert(), so a stray -DNDEBUG
# in the environment would turn every one of them into a no-op that "passes".
parameter_values_test: parameter_values_test.cpp \
                       $(SRC_DIR)/params/parameter_values.cpp \
                       $(SRC_DIR)/params/parameter_values.h \
                       $(SRC_DIR)/params/param_specs.h
	$(CXX) $(CXXFLAGS) -UNDEBUG -o $@ \
	    parameter_values_test.cpp $(SRC_DIR)/params/parameter_values.cpp

usb_midi_parser_test: usb_midi_parser_test.cpp \
                      $(SRC_DIR)/midi/usb_midi_parser.cpp \
                      $(SRC_DIR)/midi/usb_midi_parser.h \
                      $(SRC_DIR)/midi/midi_types.h
	$(CXX) $(CXXFLAGS) -UNDEBUG -o $@ \
	    usb_midi_parser_test.cpp $(SRC_DIR)/midi/usb_midi_parser.cpp

monitor_state_test: monitor_state_test.cpp \
                    $(SRC_DIR)/midi/monitor_state.cpp \
                    $(SRC_DIR)/midi/monitor_state.h \
                    $(SRC_DIR)/midi/usb_midi_parser.cpp \
                    $(SRC_DIR)/midi/midi_types.h
	$(CXX) $(CXXFLAGS) -UNDEBUG -o $@ \
	    monitor_state_test.cpp $(SRC_DIR)/midi/monitor_state.cpp \
	    $(SRC_DIR)/midi/usb_midi_parser.cpp

run: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; echo; done
	@echo "all esp32 host tests passed"

clean:
	rm -f $(TESTS)
	rm -rf $(TESTS:=.dSYM)
