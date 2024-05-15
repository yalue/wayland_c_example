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

#define WAYLAND_DISPLAY_OBJECT_ID (1)
#define WAYLAND_DISPLAY_GET_REGISTRY_OPCODE (1)
#define WAYLAND_REGISTRY_GLOBAL_EVENT (0)
#define WAYLAND_DISPLAY_ERROR_EVENT (0)
#define IMAGE_WIDTH (256)
#define IMAGE_HEIGHT (256)
#define COLOR_CHANNELS (4)

// Will be set to nonzero if the application should exit.
static int should_exit = 0;

// Holds various IDs and such we use for the window.
typedef struct {
  // The FD for the connection to Wayland.
  int socket_fd;
  // The FD for the shared memory object containing the image buffer.
  int shm_fd;
  // The ID of the display registry used by wayland.
  uint32_t registry_id;
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

/*
static void SaveDebugFile(int file_id, uint8_t *data, int size) {
  char filename[256];
  snprintf(filename, sizeof(filename) - 1, "debug_dump_%d.bin", file_id);
  printf("Saving %d bytes to %s\n", size, filename);
  FILE *f = fopen(filename, "wb");
  if (!f) {
    printf("Error opening debug file %s: %s\n", filename, strerror(errno));
    return;
  }
  if (fwrite(data, size, 1, f) != 1) {
    printf("Failed writing %d bytes to %s: %s\n", size, filename,
      strerror(errno));
  }
  fclose(f);
}
*/

static void AppendUint32(uint8_t *buffer, size_t *current_offset, uint32_t v) {
  *((uint32_t *) (buffer + *current_offset)) = v;
  *current_offset += 4;
}

static uint32_t ReadUint32(uint8_t *buffer, size_t *current_offset) {
  uint32_t to_return = *((uint32_t *) (buffer + *current_offset));
  *current_offset += 4;
  return to_return;
}

// Rounds v up to the next multiple of 4.
static uint32_t RoundUp4(uint32_t v) {
  while (v & 3) v++;
  return v;
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
  printf("Mapping %d-byte shared memory file %s (FD %d)\n", (int) s->image_buffer_size, shm_path, s->shm_fd);
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

// Handles a single event received on the wayland socket. Receives the object's
// ID, opcode, the (non-padded) size of the message payload and the payload
// itself, which must be ignored if payload_size is 0.
static int HandleWaylandEvent(ApplicationState *s, ParsedWaylandEvent *e) {
  size_t payload_offset = 0;
  uint32_t name, interface_version = 0;
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
    return 1;
  }

  if ((e->object_id == WAYLAND_DISPLAY_OBJECT_ID) &&
    (e->opcode == WAYLAND_DISPLAY_ERROR_EVENT)) {
    PrintErrorEventInfo(e);
    return 0;
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

