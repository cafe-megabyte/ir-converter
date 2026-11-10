# Offline builds require only Apple's Command Line Tools.
.PHONY: all web check serve clean rebuild-toolchain
.NOTPARALLEL:
all web check serve rebuild-toolchain:
	@/bin/sh build-support/scripts/build.sh $@

clean:
	@/bin/sh build-support/scripts/build.sh clean
