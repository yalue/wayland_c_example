Connecting to Wayland Using C
=============================

This repository contains my code following along with the nice tutorial for
interacting with Wayland [here](https://gaultier.github.io/blog/wayland_from_scratch.html).

Things I Wish I Knew When Starting
----------------------------------

 - It's not immediately obvious from the blogpost linked above how to look up
   opcode numbers.  The intention seems to be to autogenerate them from the
   wayland protocol XML. The opcode number is simply the index of the
   particular `<request>` child under the `<interface>` of interest. For
   example, say you want to find the opcode number for the `get_xdg_surface`
   method. You'll need to find the appropriate XML file, then find the
   `<interface name="xdg_wm_base">` object. Then count the
   `<request name="..">` objects until you find `<request name="get_xdg_surface ...>`
   entry. At the time of writing, this is the 3rd entry, making it index 2. So,
   the opcode for `get_xdg_surface` is `2`.

 - Related to the prior point, you'll probably want to refer to the Wayland
   XML. This wasn't available by default on my Ubuntu system. I had to install
   `sudo apt install wayland-protocols` first. Afterwards, I could look up the
   core wayland objects in `/usr/share/wayland/wayland.xml` and the XDG-related
   objects in `/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml`.

