
PROJECT_ROOT = ../../..
PROJECT_NAME = hphp_cpp_eval

SYSTEM=$(PROJECT_ROOT)/src/lib/system

AUTO_SOURCES_RECURSIVE = 1
EVAL_DIR=.

include gen.mk
include $(PROJECT_ROOT)/src/rules.mk

ifdef MAC_OS_X
TARGETS = $(STATIC_LIB)
else
TARGETS = $(STATIC_LIB) $(SHARED_LIB)
endif

all: $(TARGETS)
