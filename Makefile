SRC = plugin.c
INCLUDE = -I$(QEMU_SRC)/include/qemu \
	-I/usr/include/glib-2.0 \
	-I/usr/lib/glib-2.0/include

kern_profile: $(SRC)
	gcc $(INCLUDE) $(SRC) -o $@
