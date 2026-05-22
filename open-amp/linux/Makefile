#
# The top level Makefile from which all other Makefiles are called.
# It defines the source files and the targets for each platform.
#
export PROJ_NAME = rpmsg-test

export ROOT_BUILD_PATH = build

#
# Adjust the YOCTO tool path as required
#
export YOCTO_SDK_PATH = /hdb/MediaTek/Pumpkin/RITY-Scarthgap_v25.1_Jailhouse/SDK/v25.1

export SRCS =	\
	src/main.c	\
	src/uio.c


.PHONY: all yocto_armv7 yocto_armv8 clean

all: yocto_armv7 yocto_armv8

yocto_armv7:
	@$(MAKE) -f Makefile.yocto_armv7 all

yocto_armv8:
	@$(MAKE) -f Makefile.yocto_armv8 all

clean:
	@$(MAKE) -f Makefile.yocto_armv7 clean
	@$(MAKE) -f Makefile.yocto_armv8 clean
