// pin16: streaming decoder for the compressed slot formats (own code).
// The stored stream is a plain LZ4 block (token, literals, 16-bit offset, match length, no trailing empty token)
// with offsets of at most LZ_WIN bytes. Format 1: the stream is the frame. Format 2: every line was XORed with
// the line above it before compression (undone while decoding). Decoding runs line by line, so only LZ_WIN bytes
// of history and one line are kept in RAM. The external flash must be open (xf_begin) while these functions run.
#ifndef LZ_H
#define LZ_H
#include <stdint.h>

#define LZ_WIN       2048
#define LZ_MAXLINE   64         // longest line the decoder has room for, bytes (256 pixels, 4 per byte)
int lz_begin(uint32_t addr, uint32_t len, int xor_lines, uint16_t line_bytes);   // 1 = ok
int lz_line(uint8_t *out);                                  // next line_bytes bytes of the frame; 0 = damaged or too short
int lz_check(uint32_t addr, uint32_t len, int xor_lines, uint16_t line_bytes, uint16_t lines);   // dry run: 1 if exactly `lines` lines and all bytes used
#endif
