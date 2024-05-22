#ifndef HEX_DUMP_H
#define HEX_DUMP_H
// This is a header-only implementation of a function that prints a hex dump
// to stdout, similar to the 'hd' utility.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Converts any nonprintable char to a '.'
static char ByteToAscii(uint8_t b) {
  if ((b < ' ') || (b >= '~')) return '.';
  return (char) b;
}

static void AppendHexBytes(char *dst, uint32_t *dst_offset, uint8_t v) {
  snprintf(dst + *dst_offset, 4, "%02x ", v);
  *dst_offset += 3;
}

static void AppendTextByte(char *dst, uint32_t *dst_offset, uint8_t v) {
  dst[*dst_offset] = ByteToAscii(v);
  *dst_offset += 1;
}

// The top-level function to print the hex dump.
static void PrintHexDump(uint8_t *buffer, uint32_t size_bytes,
  uint32_t start_address) {
  char hex_buffer[128];
  char text_buffer[64];
  // We'll always align the addresses we print to 16 bytes.
  uint32_t current_address = start_address & ~((uint32_t) 0xf);
  uint32_t hex_offset = 0, text_offset = 0, byte_offset = 0;
  uint32_t end_address = start_address + size_bytes;
  uint8_t b;
  memset(hex_buffer, 0, sizeof(hex_buffer));
  memset(text_buffer, 0, sizeof(text_buffer));

  while (current_address < end_address) {
    if ((current_address & 0xf) == 0) {
      printf("%08x  ", current_address);
    }
    current_address++;
    if (current_address < start_address) {
      // We're before the requested start address, output whitespace.
      memset(hex_buffer + hex_offset, ' ', 3);
      hex_offset += 3;
      AppendTextByte(text_buffer, &text_offset, ' ');
      // One space of padding between the two 8-byte hex columns.
      if ((current_address & 0xf) == 8) {
        AppendTextByte(hex_buffer, &hex_offset, ' ');
      }
    } else {
      b = buffer[byte_offset];
      byte_offset++;
      // We're past the requested start address, so print the hex bytes.
      AppendHexBytes(hex_buffer, &hex_offset, b);
      AppendTextByte(text_buffer, &text_offset, b);
      if ((current_address & 0xf) == 8) {
        AppendTextByte(hex_buffer, &hex_offset, ' ');
      }
    }
    // Output the formatted line every 16 bytes of address.
    if ((current_address & 0xf) == 0) {
      hex_buffer[hex_offset] = 0;
      text_buffer[text_offset] = 0;
      printf("%s |%s|\n", hex_buffer, text_buffer);
      hex_offset = 0;
      text_offset = 0;
    }
  }
  if ((current_address & 0xf) != 0) {
    hex_buffer[hex_offset] = 0;
    text_buffer[text_offset] = 0;
    printf("%-49s |%s|\n", hex_buffer, text_buffer);
  }
}

#endif  // HEX_DUMP_H

