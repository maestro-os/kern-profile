SRC = plugin.c

kern_profile:
	gcc -I$(QEMU_SRC)/include/qemu $(SRC) -o $@
