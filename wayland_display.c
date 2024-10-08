#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "hex_dump.h"

#define WAYLAND_DISPLAY_OBJECT_ID (1)
#define WAYLAND_DISPLAY_GET_REGISTRY_OPCODE (1)
#define WAYLAND_REGISTRY_BIND_OPCODE (0)
#define WAYLAND_REGISTRY_GLOBAL_EVENT (0)
#define WAYLAND_DISPLAY_ERROR_EVENT (0)
#define WAYLAND_SHM_FORMAT_EVENT (0)
#define XDG_WM_PING_EVENT (0)
#define XDG_SURFACE_CONFIGURE_EVENT (0)
#define XDG_TOPLEVEL_CONFIGURE_EVENT (0)
#define IMAGE_WIDTH (256)
#define IMAGE_HEIGHT (256)
#define COLOR_CHANNELS (4)

// Will be set to nonzero if the application should exit.
static int should_exit = 0;

typedef enum {
  NONE = 0,
  ACKED_CONFIGURE = 1,
  SURFACE_ATTACHED = 2,
} SurfaceState;

// Holds various IDs and such we use for the window.
typedef struct {
  // The FD for the connection to Wayland.
  int socket_fd;
  // The FD for the shared memory object containing the image buffer.
  int shm_fd;
  // The ID of the display registry used by wayland.
  uint32_t registry_id;
  // The ID bound to the global wl_shm object.
  uint32_t shm_id;
  // The ID of the shm_pool object.
  uint32_t shm_pool_id;
  // The ID of the frame buffer within the shm pool.
  uint32_t frame_buffer_id;
  // The ID bound to the global wl_compositor object.
  uint32_t compositor_id;
  // The ID bound to the global xdg_wm_base object.
  uint32_t xdg_wm_base_id;
  // The IDs of the wayland surface object and the associated xdg objects.
  uint32_t surface_id;
  uint32_t xdg_surface_id;
  uint32_t xdg_toplevel_id;
  // Will be nonzero if we're done binding to the global objects we need.
  int binding_done;
  // Will be 0 if we ack'd the initial xdg_surface.configure event.
  SurfaceState surface_state;
  // Properties of the image we'll display.
  uint32_t width;
  uint32_t height;
  uint32_t color_channels;
  uint32_t stride;
  // The actual buffer containing the image.
  uint32_t image_buffer_size;
  uint8_t *image_buffer;
} ApplicationState;

// Holds data received from the server.
typedef struct {
  uint32_t object_id;
  uint16_t opcode;
  // Holds the size of the payload, in bytes. Does not include padding or the
  // size of the header.
  uint16_t payload_size;
  // Will be NULL if payload_size is 0.
  uint8_t *payload;
} ParsedWaylandEvent;

// Frees and destroys any state held in s, including unlinking the shared
// memory objects and closing sockets.
static void CleanupState(ApplicationState *s) {
  if (s->socket_fd >= 0) {
    close(s->socket_fd);
  }
  if (s->shm_fd >= 0) {
    munmap(s->image_buffer, s->image_buffer_size);
    close(s->shm_fd);
  }

  memset(s, 0, sizeof(*s));
  s->socket_fd = -1;
  s->shm_fd = -1;
}

// Rounds v up to the next multiple of 4.
static uint32_t RoundUp4(uint32_t v) {
  while (v & 3) v++;
  return v;
}

static void AppendUint32(uint8_t *buffer, size_t *current_offset, uint32_t v) {
  *((uint32_t *) (buffer + *current_offset)) = v;
  *current_offset += 4;
}

// Appends the given string, preceded by its length, to the buffer. Prints a
// message and returns 0 if appending the string would exceed the buffer's
// capacity.
static int AppendWaylandString(uint8_t *buffer, size_t *current_offset,
  size_t buffer_capacity, char *str) {
  size_t length = strlen(str);
  size_t padded_length = RoundUp4(length + 1);
  size_t padding_length = padded_length - length;
  size_t remaining = buffer_capacity - *current_offset;
  if ((padded_length + 4) > remaining) {
    printf("Appending a string exceeds the %u bytes remaining in a buffer.\n",
      (unsigned) remaining);
    return 0;
  }
  // 1. Append the string length + 1 for null terminator.
  // 2. Append the string content
  // 3. Add the null terminator.
  AppendUint32(buffer, current_offset, length + 1);
  memcpy(buffer + *current_offset, str, length);
  *current_offset += length;
  memset(buffer + *current_offset, 0, padding_length);
  *current_offset += padding_length;
  return 1;
}

static uint32_t ReadUint32(uint8_t *buffer, size_t *current_offset) {
  uint32_t to_return = *((uint32_t *) (buffer + *current_offset));
  *current_offset += 4;
  return to_return;
}

// Assumes the current offset into the buffer is at the start of a wayland
// event. Fills dst with the event data, incrementing the current_offset to
// point past the event in the buffer.
static void ReadWaylandEvent(uint8_t *buffer, size_t *current_offset,
  ParsedWaylandEvent *dst) {
  uint32_t opcode_and_size, size_with_header;
  dst->object_id  = ReadUint32(buffer, current_offset);
  opcode_and_size = ReadUint32(buffer, current_offset);
  dst->opcode = opcode_and_size & 0xffff;
  size_with_header = opcode_and_size >> 16;
  if (size_with_header < 8) {
    printf("Got invalid wayland message size: %d\n", (int) size_with_header);
    exit(1);
  }
  dst->payload_size = size_with_header - 8;
  if (dst->payload_size == 0) {
    dst->payload = NULL;
  } else {
    dst->payload = buffer + *current_offset;
  }
  *current_offset += RoundUp4(dst->payload_size);
}

// Serializes the src event into the buffer at the given offset, including
// copying the message's payload. Updates *current_offset to point past the end
// of the serialized message.
static void WriteWaylandMessage(uint8_t *buffer, size_t *current_offset,
    ParsedWaylandEvent *src) {
  uint32_t opcode_and_size = 0;
  AppendUint32(buffer, current_offset, src->object_id);
  opcode_and_size = (src->payload_size + 8) << 16;
  opcode_and_size |= src->opcode;
  AppendUint32(buffer, current_offset, opcode_and_size);
  if (src->payload_size > 0) {
    memcpy(buffer + *current_offset, src->payload, src->payload_size);
    *current_offset += RoundUp4(src->payload_size);
  }
}

// Reads a wayland string from a buffer. A wayland string is prefixed by a
// 4-byte size, includes the null terminator, and is padded to 4 bytes. Updates
// the buffer offset to be past the end of the string and padding. Returns an
// empty null terminated string ("") if the string's length is 0.
static char* ReadWaylandString(uint8_t *buffer, size_t *current_offset) {
  char *to_return = NULL;
  uint32_t string_length = ReadUint32(buffer, current_offset);
  if (string_length == 0) return "";
  to_return = (char *) (buffer + *current_offset);
  *current_offset += RoundUp4(string_length);
  return to_return;
}

// Generates and returns a unique ID to identify messages sent and received.
// Will never return 0, and will wrap around if IDs exceed the max client ID.
static uint32_t NextWaylandID(void) {
  // IDs must be densely packed according to the spec, and ID 1 already belongs
  // to the display object, so the first ID we'll return will be 2.
  static uint32_t next_id = 1;
  next_id++;
  // IDs above 0xfeffffff are reserved for the server.
  if (next_id > 0xfeffffff) {
    printf("Error: Allocated too many client-side wayland IDs.\n");
    exit(1);
  }
  return next_id;
}

// Returns the FD for the wayland Unix socket. Prints a message and returns -1
// if an error occurs.
static int GetWaylandConnection(void) {
  struct sockaddr_un address;
  char *xdg_dir = getenv("XDG_RUNTIME_DIR");
  char *display_name = NULL;
  int fd, result;
  if (!xdg_dir) {
    printf("The XDG_RUNTIME_DIR environment variable was not set.\n");
    return -1;
  }
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;

  // If WAYLAND_DISPLAY is not set, fall back to trying to connect to wayland-0
  display_name = getenv("WAYLAND_DISPLAY");
  if (!display_name) {
    display_name = "wayland-0";
  }
  snprintf(address.sun_path, sizeof(address.sun_path) - 1, "%s/%s", xdg_dir,
    display_name);
  printf("Connecting to display path: %s\n", address.sun_path);
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    printf("Error creating socket: %s\n", strerror(errno));
    return -1;
  }
  result = connect(fd, (struct sockaddr *) &address, sizeof(address));
  if (result != 0) {
    printf("Error connecting to %s: %s\n", address.sun_path, strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

// Gets the wayland display object registry ID. Returns 0 on error.
static uint32_t GetWaylandDisplayRegistry(ApplicationState *s) {
  ParsedWaylandEvent msg;
  uint8_t buffer[128];
  size_t buffer_offset = 0;
  uint32_t wayland_id = NextWaylandID();
  size_t result;
  msg.object_id = WAYLAND_DISPLAY_OBJECT_ID;
  msg.opcode = WAYLAND_DISPLAY_GET_REGISTRY_OPCODE;
  msg.payload_size = sizeof(wayland_id);
  msg.payload = (uint8_t *) (&wayland_id);
  WriteWaylandMessage(buffer, &buffer_offset, &msg);
  result = send(s->socket_fd, buffer, buffer_offset, MSG_DONTWAIT);
  if (result != buffer_offset) {
    printf("Error sending message: %s\n", strerror(errno));
    return 0;
  }
  s->registry_id = wayland_id;
  return 1;
}

// Binds the global object with the given numeric "name" to a new ID and
// returns the ID.
static uint32_t WaylandRegistryBind(ApplicationState *s, uint32_t name,
  char *interface, uint32_t version) {
  ParsedWaylandEvent msg;
  uint8_t buffer[256];
  uint8_t payload[248];
  size_t buffer_offset = 0;
  size_t payload_offset = 0;
  uint32_t new_id = 0;
  size_t result;
  new_id = NextWaylandID();

  // The args:
  //  1. Numeric name
  //  2. Interface string (length + content)
  //  3. Version
  //  4. The ID to bind the object to
  // WHY IS THIS NOT WHAT IT SAYS IN WAYLAND.XML???? WHY IS THE "DOCUMENTATION"
  // APPARENTLY SOME RANDOM BLOG POSTS?
  AppendUint32(payload, &payload_offset, name);
  if (!AppendWaylandString(payload, &payload_offset, sizeof(payload),
    interface)) {
    printf("Error copying wayland interface name string.\n");
    return 0;
  }
  AppendUint32(payload, &payload_offset, version);
  AppendUint32(payload, &payload_offset, new_id);

  msg.object_id = s->registry_id;
  msg.opcode = WAYLAND_REGISTRY_BIND_OPCODE;
  msg.payload_size = payload_offset;
  msg.payload = payload;
  WriteWaylandMessage(buffer, &buffer_offset, &msg);
  result = send(s->socket_fd, buffer, buffer_offset, 0);
  if (result != buffer_offset) {
    printf("Error sending registry bind message: %s\n", strerror(errno));
    return 0;
  }
  return new_id;
}

// Generates a random path for the shared memory object, opens it, and maps it
// into the image buffer. Returns 0 on error.
static int OpenSharedMemoryObject(ApplicationState *s) {
  char shm_path[256];
  memset(shm_path, 0, sizeof(shm_path));
  int t = (time(NULL) % 0xffffff);
  snprintf(shm_path, sizeof(shm_path) - 1, "wl_shm_%d", t);
  s->shm_fd = shm_open(shm_path, O_RDWR | O_EXCL | O_CREAT, 0600);
  if (s->shm_fd <= -1) {
    printf("Error creating %s: %s\n", shm_path, strerror(errno));
    return 0;
  }
  // We can unlink this now since we'll only need the FD.
  if (shm_unlink(shm_path) != 0) {
    printf("Error unlinking %s: %s\n", shm_path, strerror(errno));
    return 0;
  }
  if (ftruncate(s->shm_fd, s->image_buffer_size) != 0) {
    printf("Error setting size of %s to %d: %s\n", shm_path,
      (int) s->image_buffer_size, strerror(errno));
    return 0;
  }
  s->image_buffer = mmap(NULL, s->image_buffer_size,
    PROT_READ | PROT_WRITE, MAP_SHARED, s->shm_fd, 0);
  if (s->image_buffer == MAP_FAILED) {
    printf("Error mapping shared image buffer: %s\n", strerror(errno));
    return 0;
  }
  return 1;
}

static void SignalHandler(int signal_number) {
  printf("Received signal %d. Exiting.\n", signal_number);
  should_exit = 1;
}

// Called if e is a Wayland error event. Prints the error message.
static void PrintErrorEventInfo(ParsedWaylandEvent *e) {
  uint32_t object_id, error_code;
  char *msg = NULL;
  size_t current_offset = 0;
  if (e->payload_size <= 12) {
    printf("Got an error event, but it was only %d bytes long.\n",
      (int) e->payload_size);
    return;
  }
  object_id = ReadUint32(e->payload, &current_offset);
  error_code = ReadUint32(e->payload, &current_offset);
  msg = ReadWaylandString(e->payload, &current_offset);
  printf("Error detected on object ID %u, code %u: %s\n", (unsigned) object_id,
    (unsigned) error_code, msg);
}

// Returns 0 if we're still looking for global objects to bind to, otherwise
// returns 1.
static int BindingDone(ApplicationState *s) {
  if (s->binding_done) return 1;
  if (s->shm_id && s->compositor_id && s->xdg_wm_base_id) {
    s->binding_done = 1;
    return 1;
  }
  return 0;
}

// Creates and sets s->surface_id.
static int CreateWLSurface(ApplicationState *s) {
  ParsedWaylandEvent msg;
  uint8_t buffer[64];
  size_t result, buffer_offset = 0;
  s->surface_id = NextWaylandID();
  msg.payload = (uint8_t *) &(s->surface_id);
  msg.payload_size = sizeof(uint32_t);
  msg.object_id = s->compositor_id;
  msg.opcode = 0;
  WriteWaylandMessage(buffer, &buffer_offset, &msg);
  result = send(s->socket_fd, buffer, buffer_offset, 0);
  if (result != buffer_offset) {
    printf("Error sending create-surface message: %s\n", strerror(errno));
    return 0;
  }
  printf("s->surface_id = %d\n", (int) s->surface_id);
  return 1;
}

// Creates and sets s->xdg_surface_id. Must be called after CreateWLSurface.
static int CreateXDGSurface(ApplicationState *s) {
  ParsedWaylandEvent msg;
  uint8_t buffer[64];
  size_t result, buffer_offset = 0;
  uint32_t args[2];
  s->xdg_surface_id = NextWaylandID();
  args[0] = s->xdg_surface_id;
  args[1] = s->surface_id;
  msg.payload = (uint8_t *) args;
  msg.payload_size = sizeof(args);
  msg.object_id = s->xdg_wm_base_id;
  // xdg_wm_base.2 = get_xdg_surface
  msg.opcode = 2;
  WriteWaylandMessage(buffer, &buffer_offset, &msg);
  result = send(s->socket_fd, buffer, buffer_offset, 0);
  if (result != buffer_offset) {
    printf("Error getting xdg surface: %s\n", strerror(errno));
    return 0;
  }
  printf("s->xdg_surface_id = %d\n", (int) s->xdg_surface_id);
  return 1;
}

// Creates and sets s->xdg_toplevel_id. Must be called after CreateXDGSurface.
static int GetXDGTopLevel(ApplicationState *s) {
  ParsedWaylandEvent msg;
  uint8_t buffer[64];
  size_t result, buffer_offset = 0;
  s->xdg_toplevel_id = NextWaylandID();
  msg.payload = (uint8_t *) &(s->xdg_toplevel_id);
  msg.payload_size = sizeof(uint32_t);
  msg.object_id = s->xdg_surface_id;
  // xdg_surface.1 = get_toplevel
  msg.opcode = 1;
  WriteWaylandMessage(buffer, &buffer_offset, &msg);
  result = send(s->socket_fd, buffer, buffer_offset, 0);
  if (result != buffer_offset) {
    printf("Error getting xdg toplevel: %s\n", strerror(errno));
    return 0;
  }
  printf("s->xdg_toplevel_id = %d\n", (int) s->xdg_toplevel_id);
  return 1;
}

static int CreateSurface(ApplicationState *s) {
  if (!CreateWLSurface(s)) return 0;
  if (!CreateXDGSurface(s)) return 0;
  if (!GetXDGTopLevel(s)) return 0;
  return 1;
}

// Wraps the sendmsg call to include the shm_fd descriptor as ancillary data.
static int SendMsgWithShmDescriptor(ApplicationState *s, uint8_t *msg_buffer,
  size_t msg_size) {
  uint8_t control_buffer[CMSG_SPACE(sizeof(int))];
  struct iovec io;
  struct msghdr message_info;
  struct cmsghdr *control_info = NULL;
  ssize_t result;
  memset(&message_info, 0, sizeof(message_info));
  memset(control_buffer, 0, sizeof(control_buffer));
  memset(&io, 0, sizeof(io));

  // Set up the entry to send the "normal" message content.
  io.iov_base = msg_buffer;
  io.iov_len = msg_size;
  message_info.msg_iov = &io;
  message_info.msg_iovlen = 1;

  // Set up the control data to send the FD.
  message_info.msg_control = control_buffer;
  message_info.msg_controllen = sizeof(control_buffer);
  control_info = CMSG_FIRSTHDR(&message_info);
  control_info->cmsg_level = SOL_SOCKET;
  control_info->cmsg_type = SCM_RIGHTS;
  control_info->cmsg_len = CMSG_LEN(sizeof(int));
  *((int *) CMSG_DATA(control_info)) = s->shm_fd;

  printf("Message sent when creating shm pool:\n");
  PrintHexDump((uint8_t *) io.iov_base, io.iov_len, 0);

  // Actually send the FD and message data.
  result = sendmsg(s->socket_fd, &message_info, 0);
  if (result < 0) {
    printf("Error sending message with FD: %s\n", strerror(errno));
    return 0;
  }
  return 1;
}

// Sends the message to create the shm_pool object. Requires a bunch of socket
// boilderplate to send the shm_fd, so this gets a separate function.
static int CreateShmPool(ApplicationState *s) {
  ParsedWaylandEvent msg;
  uint8_t buffer[256];
  size_t buffer_offset = 0;
  uint32_t args[2];
  uint32_t shm_pool_id = NextWaylandID();
  // wayland.xml includes the FD in the args, but the blog post code does not.
  // This version seems to work on my systems.
  args[0] = shm_pool_id;
  args[1] = s->image_buffer_size;
  // wl_shm.create_pool = opcode 0
  msg.object_id = s->shm_id;
  msg.opcode = 0;
  msg.payload = (uint8_t *) args;
  msg.payload_size = sizeof(args);
  WriteWaylandMessage(buffer, &buffer_offset, &msg);
  if (!SendMsgWithShmDescriptor(s, buffer, buffer_offset)) {
    printf("Error sending shm_pool.create message.\n");
    return 0;
  }
  s->shm_pool_id = shm_pool_id;
  return 1;
}

// Calls the create_buffer method to set up the shared memory pool as a frame
// buffer.
static int CreateFrameBuffer(ApplicationState *s) {
  ParsedWaylandEvent msg;
  uint8_t buffer[256];
  size_t buffer_offset = 0;
  size_t result;
  uint32_t args[6];
  uint32_t buffer_id = NextWaylandID();

  // See wayland.xml for the args order.
  args[0] = buffer_id;
  args[1] = 0;  // offset in shm buffer
  args[2] = s->width;
  args[3] = s->height;
  args[4] = s->stride;
  args[5] = 0;  // argb8888

  // shm_pool.create_buffer = opcode 0
  msg.object_id = s->shm_pool_id;
  msg.opcode = 0;
  msg.payload = (uint8_t *) args;
  msg.payload_size = sizeof(args);
  WriteWaylandMessage(buffer, &buffer_offset, &msg);
  result = send(s->socket_fd, buffer, buffer_offset, 0);
  if (result != buffer_offset) {
    printf("Error sending create-buffer message: %s\n", strerror(errno));
    return 0;
  }
  s->frame_buffer_id = buffer_id;
  return 1;
}

// To be used after creating the frame buffer. Attaches the frame buffer to the
// wl_surface.
static int AttachBuffer(ApplicationState *s) {
  ParsedWaylandEvent msg;
  uint8_t buffer[128];
  size_t buffer_offset = 0;
  size_t result;
  uint32_t args[3];

  // Frame buffer, x, y
  args[0] = s->frame_buffer_id;
  args[1] = 0;
  args[2] = 0;

  // surface.attach = opcode 1
  msg.object_id = s->surface_id;
  msg.opcode = 1;
  msg.payload = (uint8_t *) args;
  msg.payload_size = sizeof(args);

  WriteWaylandMessage(buffer, &buffer_offset, &msg);
  result = send(s->socket_fd, buffer, buffer_offset, 0);
  if (result != buffer_offset) {
    printf("Error sending surface attach message: %s\n", strerror(errno));
    return 0;
  }
  return 1;
}

// Signals that the surface is ready to display.
static int CommitSurface(ApplicationState *s) {
  ParsedWaylandEvent msg;
  uint8_t buffer[64];
  size_t buffer_offset = 0;
  size_t result;
  msg.object_id = s->surface_id;
  msg.opcode = 6;
  msg.payload_size = 0;
  msg.payload = NULL;
  WriteWaylandMessage(buffer, &buffer_offset, &msg);
  result = send(s->socket_fd, buffer, buffer_offset, 0);
  if (result != buffer_offset) {
    printf("Error sending surface commit message: %s\n", strerror(errno));
    return 0;
  }
  return 1;
}

// To be called after a configure is ACKED in order to render a frame. Sets up
// the shm buffers if they aren't already set up.
static int RenderFrame(ApplicationState *s) {
  uint32_t x, y, row_offset, pixel_offset;
  if (s->shm_pool_id == 0) {
    if (!CreateShmPool(s)) {
      printf("Error creating shm_pool.\n");
      return 0;
    }
  }
  if (s->frame_buffer_id == 0) {
    if (!CreateFrameBuffer(s)) {
      printf("Error creating frame buffer.\n");
      return 0;
    }
  }

  // Fill the image buffer with opaque red.
  memset(s->image_buffer, 0, s->image_buffer_size);
  row_offset = 0;
  for (y = 0; y < s->height; y++) {
    for (x = 0; x < s->width; x++) {
      pixel_offset = row_offset + (x * 4);
      // Blue
      s->image_buffer[pixel_offset] = 0xaa;
      // Green
      s->image_buffer[pixel_offset + 1] = 0x10;
      // Red
      s->image_buffer[pixel_offset + 2] = 0x55;
      // Alpha
      s->image_buffer[pixel_offset + 3] = 0xff;
    }
    row_offset += s->stride;
  }

  if (!AttachBuffer(s)) {
    printf("Error attaching buffer to surface.\n");
    return 0;
  }
  if (!CommitSurface(s)) {
    printf("Error committing surface.\n");
    return 0;
  }
  s->surface_state = SURFACE_ATTACHED;
  return 1;
}

// Responds to an xdg "ping" to check that the application is alive.
static int SendXDGPong(ApplicationState *s, uint32_t ping_serial) {
  ParsedWaylandEvent msg;
  uint8_t buffer[128];
  size_t buffer_offset = 0;
  size_t result;
  uint32_t arg = ping_serial;
  msg.object_id = s->xdg_wm_base_id;
  msg.payload_size = sizeof(uint32_t);
  msg.payload = (uint8_t *) &arg;
  // xdg_wm_base.3 = pong
  msg.opcode = 3;
  WriteWaylandMessage(buffer, &buffer_offset, &msg);
  result = send(s->socket_fd, buffer, buffer_offset, 0);
  if (result != buffer_offset) {
    printf("Error sending XDG WM pong: %s\n", strerror(errno));
    return 0;
  }
  return 1;
}

// Responds to xdg_surface "configure" events. Similar to SendXDGPong.
static int AckXDGSurfaceConfigure(ApplicationState *s, uint32_t serial) {
  ParsedWaylandEvent msg;
  uint8_t buffer[64];
  size_t result, buffer_offset = 0;
  uint32_t arg = serial;
  msg.object_id = s->xdg_surface_id;
  msg.payload_size = sizeof(uint32_t);
  msg.payload = (uint8_t *) &arg;
  // xdg_surface.4 = ack_configure
  msg.opcode = 4;
  WriteWaylandMessage(buffer, &buffer_offset, &msg);
  result = send(s->socket_fd, buffer, buffer_offset, 0);
  if (result != buffer_offset) {
    printf("Error sending xdg_surface.ack_configure: %s\n", strerror(errno));
    return 0;
  }
  return 1;
}

// Handles a single event received on the wayland socket. Receives the object's
// ID, opcode, the (non-padded) size of the message payload and the payload
// itself, which must be ignored if payload_size is 0.
static int HandleWaylandEvent(ApplicationState *s, ParsedWaylandEvent *e) {
  size_t payload_offset = 0;
  uint32_t name, interface_version = 0;
  int result;
  char *interface_name = NULL;

  // The global registry can produce two events: announcing an object is
  // available, and announcing a global object is removed.
  if ((e->object_id == s->registry_id) &&
    (e->opcode == WAYLAND_REGISTRY_GLOBAL_EVENT)) {
    name = ReadUint32(e->payload, &payload_offset);
    interface_name = ReadWaylandString(e->payload, &payload_offset);
    interface_version = ReadUint32(e->payload, &payload_offset);
    printf("Found interface %s: name %u, version %u\n", interface_name,
      (unsigned) name, (unsigned) interface_version);
    if (strcmp("wl_shm", interface_name) == 0) {
      s->shm_id = WaylandRegistryBind(s, name, interface_name,
        interface_version);
      if (!s->shm_id) {
        printf("Error binding wl_shm object.\n");
        return 0;
      } else {
        printf("  -> Bound to ID %u\n", (unsigned) s->shm_id);
      }
    }
    if (strcmp("xdg_wm_base", interface_name) == 0) {
      s->xdg_wm_base_id = WaylandRegistryBind(s, name, interface_name,
        interface_version);
      if (!s->xdg_wm_base_id) {
        printf("Error binding xdg_wm_base object.\n");
        return 0;
      } else {
        printf("  -> Bound to ID %u\n", (unsigned) s->xdg_wm_base_id);
      }
    }
    if (strcmp("wl_compositor", interface_name) == 0) {
      s->compositor_id = WaylandRegistryBind(s, name, interface_name,
        interface_version);
      if (!s->compositor_id) {
        printf("Error binding wl_compositor object.\n");
        return 0;
      } else {
        printf("  -> Bound to ID %u\n", (unsigned) s->compositor_id);
      }
    }
    return 1;
  }

  if ((e->object_id == WAYLAND_DISPLAY_OBJECT_ID) &&
    (e->opcode == WAYLAND_DISPLAY_ERROR_EVENT)) {
    PrintErrorEventInfo(e);
    return 0;
  }

  // "ping" event from the xdg_wm_base
  if ((e->object_id == s->xdg_wm_base_id) &&
    (e->opcode == XDG_WM_PING_EVENT)) {
    if (e->payload_size != sizeof(uint32_t)) {
      printf("Incorrect xdg ping payload size: %d\n", (int) e->payload_size);
      return 0;
    }
    return SendXDGPong(s, *((uint32_t *) e->payload));
  }

  // The surface configure message from xdg_surface must be ack'd
  if ((e->object_id == s->xdg_surface_id) &&
    (e->opcode == XDG_SURFACE_CONFIGURE_EVENT)) {
    if (e->payload_size != sizeof(uint32_t)) {
      printf("Incorrect xdg_surface configure payload size: %d\n",
        (int) e->payload_size);
      return 0;
    }
    result = AckXDGSurfaceConfigure(s, *((uint32_t *) e->payload));
    if (result == 0) return 0;
    s->surface_state = ACKED_CONFIGURE;
    return 1;
  }

  // The configure message from xdg_toplevel must also be handled. The current
  // version in xdg-shell.xml says that this requires an ack, but the blog post
  // version never sends an ack...
  if ((e->object_id == s->xdg_toplevel_id) &&
    (e->opcode == XDG_TOPLEVEL_CONFIGURE_EVENT)) {
    if (e->payload_size < 8) {
      printf("Invalid payload size for xdg_toplevel configure: %d\n",
        (int) e->payload_size);
    }
    printf("Got xdg toplevel configure event. W=%d, H=%d\n",
      *((int *) e->payload), *((int *) (e->payload + 4)));
    return 1;
  }

  // These are informational messages about supported pixel formats.
  if ((e->object_id == s->shm_id) &&
    (e->opcode == WAYLAND_SHM_FORMAT_EVENT)) {
    if (e->payload_size != sizeof(uint32_t)) {
      printf("Incorrect wl_shm.format payload size: %d\n",
        (int) e->payload_size);
      return 0;
    }
    printf("Supported pixel format: 0x%08x\n", *((uint32_t *) e->payload));
    return 1;
  }

  printf("Handling opcode %d on object %u is not supported!\n",
    (int) e->opcode, (unsigned) e->object_id);
  return 0;
}

// Reads events from the buffer until buffer_size bytes have been processed.
static int ProcessWaylandEvents(ApplicationState *s, uint8_t *buffer,
    uint32_t buffer_size) {
  size_t buffer_offset = 0;
  ParsedWaylandEvent event;
  while (buffer_offset < buffer_size) {
    ReadWaylandEvent(buffer, &buffer_offset, &event);
    if ((buffer_offset - 1) > buffer_size) {
      printf("A message with a body of %d bytes overflowed the buffer "
        "containing %d bytes.\n", (int) event.payload_size, (int) buffer_size);
      return 0;
    }
    if (!HandleWaylandEvent(s, &event)) {
      printf("Error handling Wayland op %u on object %u.\n",
        (unsigned) event.opcode, (unsigned) event.object_id);
      return 0;
    }
  }
  return 1;
}

// Reads from the socket and handles events until signalled or an error occurs.
// Returns 0 if an error caused an exit, and 1 otherwise.
static int EventLoop(ApplicationState *s) {
  // This really should probably be a ring buffer in case a message is split
  // between two reads or the buffer overflows, but whatever. The article I
  // followed ignores this issue for simplicity as well.
  uint8_t recv_buffer[4096];
  memset(recv_buffer, 0, sizeof(recv_buffer));
  ssize_t bytes_read = 0;
  while (!should_exit) {
    bytes_read = recv(s->socket_fd, recv_buffer, sizeof(recv_buffer), 0);
    if (bytes_read < 0) {
      printf("Error receiving wayland message: %s\n", strerror(errno));
      return 0;
    }
    if (!ProcessWaylandEvents(s, recv_buffer, bytes_read)) {
      printf("Error handling wayland messages.\n");
      return 0;
    }
    if (BindingDone(s) && !s->surface_id) {
      if (!CreateSurface(s)) {
        printf("Error creating surface.\n");
        return 0;
      }
      if (!CommitSurface(s)) {
        printf("Error initially committing surface.\n");
        return 0;
      }
      printf("Created surface.\n");
    }
    if (s->surface_state == ACKED_CONFIGURE) {
      if (!RenderFrame(s)) {
        printf("Error rendering a frame.\n");
        return 0;
      }
    }
  }
  return 1;
}

int main(int argc, char **argv) {
  ApplicationState state;
  struct sigaction signal_action;
  int result;
  should_exit = 0;
  memset(&state, 0, sizeof(state));
  memset(&signal_action, 0, sizeof(signal_action));
  state.socket_fd = -1;
  state.shm_fd = -1;
  state.socket_fd = GetWaylandConnection();
  if (state.socket_fd <= 0) return 1;

  // We'll use the xrgb8888 color format, which the specification guarantees
  // will be supported.
  state.width = IMAGE_WIDTH;
  state.height = IMAGE_HEIGHT;
  state.stride = state.width * COLOR_CHANNELS;
  state.image_buffer_size = state.stride * state.height;

  // Map shared memory and get the display registry.
  if (!OpenSharedMemoryObject(&state)) {
    CleanupState(&state);
    return 1;
  }
  if (!GetWaylandDisplayRegistry(&state)) {
    CleanupState(&state);
    return 1;
  }

  // Set the signal handler so we can break out of the loop with SIGINT.
  signal_action.sa_handler = SignalHandler;
  if (sigaction(SIGINT, &signal_action, NULL) != 0) {
    printf("Error setting SIGINT handler: %s\n", strerror(errno));
    CleanupState(&state);
    return 1;
  }

  // Run the event loop until exit.
  printf("Running. Press Ctrl+C to exit.\n");
  result = EventLoop(&state);
  if (!result) {
    printf("The event loop exited with an error.\n");
  } else {
    printf("The event loop ended normally.\n");
  }

  // Unmap state, close socket, etc, regardless of whether the exit was due to
  // an error.
  CleanupState(&state);
  return 0;
}

