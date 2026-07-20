TARGET      = v0
CPP_SOURCES = main.cpp

LIBDAISY_DIR = ../libDaisy
DAISYSP_DIR  = ../DaisySP

# ReverbSc lives in the LGPL tree; enable it (adds the include path, the
# USE_DAISYSP_LGPL define, and links -ldaisysp-lgpl).
USE_DAISYSP_LGPL = 1

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
