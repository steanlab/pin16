// pin16: the NFC chip of the tag (3ALogics TNB132M, a Type 3 tag with an I2C side). Own code.
// The chip is used for two things only: the field detect pin tells that a phone came near (hw.c), and the NDEF record in the chip
// (a URL, a text or a Wi-Fi credential, see ndef.h) can be read and rewritten over I2C. The bus exchanges were recorded on three tags (docs/hardware.md); the sequence
// below is the one the stock firmware uses and the one the recordings show.
//   0x30 : write 21 04, read register 25 (59 on all tags seen); 0x43 : read block 0x30   - wake-up exchange
//   0x48 : block 0 = attribute information (16 bytes: version 0x10, Nbr, Nbw, Nmaxb 0x003C, RWFlag, Ln (3 bytes), checksum)
//   0x40 : NDEF data blocks of 16 bytes at sub-addresses 0x10, 0x20, ...
#ifndef NFC_H
#define NFC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ndef.h"

#define NFC_URL_MAX   NDEF_URL_MAX      // longest URL that is written
#define NFC_SHOW_MAX  120               // longest text kept for display

typedef enum { NFC_OK = 0, NFC_E_BUS, NFC_E_ARG, NFC_E_VERIFY, NFC_E_BUSY } nfc_status_t;

typedef struct {
  bool     present;            // the chip answered at start-up
  uint8_t  reg25;              // value of register 0x25 (59 on the tags seen)
  uint8_t  ai[16];             // attribute information block
  uint16_t ndef_len;           // Ln from the block
  uint16_t cap;                // bytes the chip holds for a message (Nmaxb * 16 from the attribute block), 0 = not known
  uint8_t  kind;               // ndef_kind_t of the first record
  char     text[NFC_SHOW_MAX + 1]; // what to show: the URL, the text or the network name (never a key); empty if there is nothing to show
} nfc_info_t;

bool               nfc_probe(void);          // power the chip, read, power off. Called once at start-up (about 100 ms)
const nfc_info_t  *nfc_info(void);
bool               nfc_busy(void);           // true while the bus is in use (the field detect interrupt is ignored then)
// Writes one NDEF record (a URL, a text, or a Wi-Fi credential: a = URL / text / network name, b = key, auth = ndef_auth_t) and checks it
// by reading it back. NFC_E_ARG if an argument is not allowed or the message does not fit the chip. The caller decides whether this is allowed.
nfc_status_t       nfc_write(ndef_kind_t kind, const char *a, const char *b, uint8_t auth);
nfc_status_t       nfc_write_url(const char *url);

#endif
