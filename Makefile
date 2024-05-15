.PHONY: all clean

all: wayland_display

wayland_display: wayland_display.c
	gcc -O3 -Wall -Werror -g -fPIC -o wayland_display wayland_display.c -lrt

clean:
	rm -f wayland_display

