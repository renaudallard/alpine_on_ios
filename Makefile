.PHONY: all emu test rootfs clean

all: emu

emu:
	$(MAKE) -C emu

test:
	$(MAKE) -C emu test

rootfs:
	./rootfs/download_rootfs.sh

clean:
	$(MAKE) -C emu clean
