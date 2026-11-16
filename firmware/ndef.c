// pin16: NDEF messages, see ndef.h. Own code.
#include <string.h>
#include "ndef.h"

static const char *const uri_prefix[] = { "", "http://www.", "https://www.", "http://", "https://" };
static const char wsc_type[] = "application/vnd.wfa.wsc";         // 23 characters

// ---- builders ----

static bool printable(const char *s, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    if ((uint8_t)s[i] < 32 || (uint8_t)s[i] > 126) return false;
  }
  return true;
}

// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF, no control characters (below 0x20, 0x7F).
static bool utf8_ok(const char *s, size_t n)
{
  size_t i = 0;
  while (i < n) {
    uint8_t c = (uint8_t)s[i];
    if (c < 0x20 || c == 0x7F) return false;
    if (c < 0x80) { i++; continue; }
    size_t k;
    uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) { k = 1; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { k = 2; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { k = 3; cp = c & 0x07; }
    else return false;
    if (i + k >= n) return false;                       // the continuation bytes must all be inside the string
    for (size_t j = 1; j <= k; j++) {
      uint8_t t = (uint8_t)s[i + j];
      if ((t & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (t & 0x3F);
    }
    if ((k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += k + 1;
  }
  return true;
}

size_t ndef_uri(uint8_t *out, const char *url)
{
  size_t n = url ? strlen(url) : 0;
  if (n == 0 || n > NDEF_URL_MAX || !printable(url, n)) return 0;
  out[0] = 0xD1; out[1] = 0x01; out[2] = (uint8_t)(1 + n); out[3] = 'U'; out[4] = 0x00;
  memcpy(out + 5, url, n);
  return 5 + n;
}

size_t ndef_text(uint8_t *out, const char *utf8)
{
  size_t n = utf8 ? strlen(utf8) : 0;
  if (n == 0 || n > NDEF_TEXT_MAX || !utf8_ok(utf8, n)) return 0;
  out[0] = 0xD1; out[1] = 0x01; out[2] = (uint8_t)(3 + n); out[3] = 'T';
  out[4] = 0x02;                                        // UTF-8, language code of 2 characters
  out[5] = 'e'; out[6] = 'n';
  memcpy(out + 7, utf8, n);
  return 7 + n;
}

static uint8_t *tlv(uint8_t *p, uint16_t id, const uint8_t *v, size_t n)
{
  *p++ = (uint8_t)(id >> 8); *p++ = (uint8_t)id; *p++ = (uint8_t)(n >> 8); *p++ = (uint8_t)n;
  if (n) { memcpy(p, v, n); p += n; }
  return p;
}

size_t ndef_wifi(uint8_t *out, const char *ssid, const char *key, uint8_t auth)
{
  size_t ns = ssid ? strlen(ssid) : 0, nk = key ? strlen(key) : 0;
  if (ns == 0 || ns > NDEF_SSID_MAX || auth > NDEF_AUTH_WPA_WPA2) return 0;
  if (auth == NDEF_AUTH_OPEN) {
    if (nk != 0) return 0;
  } else if (nk < 8 || nk > NDEF_KEY_MAX || !printable(key, nk)) {
    return 0;
  }
  uint16_t at = auth == NDEF_AUTH_OPEN ? 0x0001 : auth == NDEF_AUTH_WPA2 ? 0x0020 : 0x0022;      // open, WPA2-PSK, WPA/WPA2-PSK
  uint16_t en = auth == NDEF_AUTH_OPEN ? 0x0001 : auth == NDEF_AUTH_WPA2 ? 0x0008 : 0x000C;      // none, AES, AES + TKIP
  uint8_t cred[136];                                 // 130 bytes at most (32 + 63 characters)
  uint8_t *c = cred;
  uint8_t one = 1, v2[2], mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
  c = tlv(c, 0x1026, &one, 1);                          // network index
  c = tlv(c, 0x1045, (const uint8_t *)ssid, ns);
  v2[0] = (uint8_t)(at >> 8); v2[1] = (uint8_t)at; c = tlv(c, 0x1003, v2, 2);
  v2[0] = (uint8_t)(en >> 8); v2[1] = (uint8_t)en; c = tlv(c, 0x100F, v2, 2);
  c = tlv(c, 0x1027, (const uint8_t *)key, nk);
  c = tlv(c, 0x1020, mac, 6);                           // any access point
  size_t cn = (size_t)(c - cred);
  uint8_t pay[160];
  uint8_t *p = pay;
  uint8_t ver = 0x10;
  p = tlv(p, 0x104A, &ver, 1);                          // WSC version 1.0
  p = tlv(p, 0x100E, cred, cn);                         // credential
  size_t pn = (size_t)(p - pay);
  size_t tn = sizeof(wsc_type) - 1;
  if (3 + tn + pn > NDEF_MAX) return 0;
  out[0] = 0xD2; out[1] = (uint8_t)tn; out[2] = (uint8_t)pn;      // MB, ME, SR, TNF 2 (MIME)
  memcpy(out + 3, wsc_type, tn);
  memcpy(out + 3 + tn, pay, pn);
  return 3 + tn + pn;
}

// ---- parser ----

static void show(char *what, size_t cap, const uint8_t *s, size_t n, bool keep_utf8)
{
  size_t k = 0;
  for (size_t i = 0; i < n && k + 1 < cap; i++) {
    uint8_t c = s[i];
    what[k++] = (c >= 32 && c < 127) || (keep_utf8 && c >= 0x80) ? (char)c : '?';
  }
  what[k] = 0;
}

static bool wsc_ssid(const uint8_t *d, size_t n, const uint8_t **ssid, size_t *sn, int depth)
{
  size_t i = 0;
  while (i + 4 <= n) {
    uint16_t id = (uint16_t)((d[i] << 8) | d[i + 1]);
    size_t l = (size_t)((d[i + 2] << 8) | d[i + 3]);
    i += 4;
    if (l > n - i) return false;
    if (id == 0x100E && depth == 0) {                   // the credential: look inside it once, never deeper
      if (wsc_ssid(d + i, l, ssid, sn, 1)) return true;
    } else if (id == 0x1045) {
      *ssid = d + i; *sn = l;
      return true;
    }
    i += l;
  }
  return false;
}

ndef_kind_t ndef_parse(const uint8_t *d, size_t n, char *what, size_t cap)
{
  if (cap) what[0] = 0;
  if (n < 3 || !cap) return NDEF_NONE;
  uint8_t fl = d[0];
  size_t tl = d[1], pl, i = 2;
  if (fl & 0x10) {
    pl = d[i++];
  } else {
    if (n < 6) return NDEF_NONE;
    pl = ((size_t)d[2] << 24) | ((size_t)d[3] << 16) | ((size_t)d[4] << 8) | d[5];
    i = 6;
  }
  size_t il = 0;
  if (fl & 0x08) {
    if (i >= n) return NDEF_NONE;
    il = d[i++];
  }
  if (i + tl + il + pl > n) return NDEF_NONE;
  const uint8_t *type = d + i, *pay = d + i + tl + il;
  uint8_t tnf = fl & 0x07;
  if (tnf == 1 && tl == 1 && type[0] == 'U' && pl >= 1) {
    unsigned code = pay[0];
    const char *pre = code < sizeof(uri_prefix) / sizeof(uri_prefix[0]) ? uri_prefix[code] : "";
    size_t k = 0;
    while (pre[k] && k + 1 < cap) { what[k] = pre[k]; k++; }
    for (size_t j = 1; j < pl && k + 1 < cap; j++) {
      uint8_t c = pay[j];
      what[k++] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    what[k] = 0;
    return NDEF_URI;
  }
  if (tnf == 1 && tl == 1 && type[0] == 'T' && pl >= 1) {
    size_t ll = pay[0] & 0x3F;
    if ((pay[0] & 0x80) || 1 + ll > pl) return NDEF_OTHER;          // UTF-16 text is not shown
    show(what, cap, pay + 1 + ll, pl - 1 - ll, true);
    return NDEF_TEXT;
  }
  if (tnf == 2 && tl == sizeof(wsc_type) - 1 && memcmp(type, wsc_type, tl) == 0) {
    const uint8_t *s;
    size_t sn;
    if (wsc_ssid(pay, pl, &s, &sn, 0)) show(what, cap, s, sn, true);
    return NDEF_WIFI;
  }
  return NDEF_OTHER;
}

void ndef_wipe(void *p, size_t n)
{
  volatile uint8_t *q = (volatile uint8_t *)p;
  while (n--) {
    *q++ = 0;
  }
}
