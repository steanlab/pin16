// pin16: NDEF messages for the NFC chip, no hardware access (so the PC tests can run it). Own code.
// One short record per message: a URI, a text (language "en", UTF-8) or a Wi-Fi credential (MIME application/vnd.wfa.wsc, the
// format phones read to join a network).
#ifndef NDEF_H
#define NDEF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NDEF_MAX        200      // longest message that is built or parsed, bytes
#define NDEF_URL_MAX    120      // characters
#define NDEF_TEXT_MAX   120      // bytes of UTF-8
#define NDEF_SSID_MAX   32       // bytes
#define NDEF_KEY_MAX    63       // characters (8 to 63 for a WPA pass phrase)

typedef enum { NDEF_NONE = 0, NDEF_URI = 1, NDEF_TEXT = 2, NDEF_WIFI = 3, NDEF_OTHER = 4 } ndef_kind_t;
typedef enum { NDEF_AUTH_OPEN = 0, NDEF_AUTH_WPA2 = 1, NDEF_AUTH_WPA_WPA2 = 2 } ndef_auth_t;

// Each builder returns the length of the message written to out (room for NDEF_MAX bytes), or 0 if an argument is not allowed.
size_t ndef_uri(uint8_t *out, const char *url);                                   // printable ASCII, 1..NDEF_URL_MAX, no prefix abbreviation
size_t ndef_text(uint8_t *out, const char *utf8);                                 // valid UTF-8, no control characters, 1..NDEF_TEXT_MAX bytes
size_t ndef_wifi(uint8_t *out, const char *ssid, const char *key, uint8_t auth);  // ssid 1..32 bytes; key 8..63 printable ASCII, empty for an open network

// Reads the first record of a message. kind NDEF_NONE (empty or damaged), URI, TEXT, WIFI or OTHER. what receives what the user
// should see: the full URL, the text, the network name (never the key); control characters become '?'.
ndef_kind_t ndef_parse(const uint8_t *d, size_t n, char *what, size_t cap);

// Overwrites a buffer that held a Wi-Fi key; the compiler may not drop it (volatile writes).
void ndef_wipe(void *p, size_t n);

#endif
