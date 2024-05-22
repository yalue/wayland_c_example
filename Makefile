.PHONY: all clean

all: wayland_display

wayland_display: wayland_display.c
	gcc -O2 -Wall -Werror -g -fPIC -o wayland_display wayland_display.c -lrt

clean:
	rm -f wayland_display

