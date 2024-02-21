NAME = kern-profile

SRC = plugin.c
INCLUDE = -I$(QEMU_SRC)/include/qemu \
	-I/usr/include/glib-2.0 \
	-I/usr/lib/glib-2.0/include

$(NAME).so: Makefile $(SRC)
	$(CC) $(INCLUDE) $(SRC) -shared -o $@

clean:
	rm $(NAME).so

.PHONY: clean