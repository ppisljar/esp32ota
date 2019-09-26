#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

EXTRA_COMPONENT_DIRS += ./components

PROJECT_NAME := esp32_minimal_ota
PROJECT_VER = "0.0.0.1"
CPPFLAGS += -D_GLIBCXX_USE_C99
include $(IDF_PATH)/make/project.mk
