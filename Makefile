#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := glowball

include $(IDF_PATH)/make/project.mk

install:
	idf.py build
	idf.py -p /dev/ttyUSB0 flash
	tio -b 115200 /dev/ttyUSB0
