#!/usr/bin/env python3
"""Patches the SDK file btl_storage_spiflash_sfdp.c of the Gecko bootloader (build time, nothing of the SDK is stored in this repository).

The flash of the Solum tags (Fudan, JEDEC A1 28 13) has an SFDP table with unusable values in three places; with the stock code the bootloader
computes absurd timeouts or a page size of 2^15 and fails to install an image. The patch adds three guards and makes the flash word size
equal to the page size. Every anchor must be found exactly once, otherwise the build stops (a new SDK needs a new look).
Usage: patch_sfdp.py <path to btl_storage_spiflash_sfdp.c>
"""
import sys

EDITS = [
    # 1. chip erase time: 0xFF count with multiplier 0xF means "not given": 30 s
    ("  multiplier = 2 * (multiplier + 1);\n",
     "  if (t_time == 0xFF && multiplier == 0xF) {   // pin16: field not given, assume 30 s\n"
     "    return 30000;\n"
     "  }\n"
     "  multiplier = 2 * (multiplier + 1);\n"),
    # 2. 4 KB sector erase time: 0xFF means "not given": 3 s
    ("  //[3:0] count_m\n",
     "  if (t_arr == 0xFF) {                         // pin16: field not given, assume 3 s\n"
     "    return 3000;\n"
     "  }\n"
     "  //[3:0] count_m\n"),
    # 3. page size: outside 16..8192 bytes cannot be right, use 256
    ("  uint32_t pageSize = 1U << byte1;\n",
     "  uint32_t pageSize = 1U << byte1;\n"
     "  if (pageSize > 8192 || pageSize < 16) {      // pin16: unusable value, use 256 bytes\n"
     "    pageSize = 256;\n"
     "  }\n"),
    # 4. word size of the part = page size
    ("  flashInfo.wordSizeBytes = DEVICE_WORD_SIZE;\n",
     "  flashInfo.wordSizeBytes = SFDP_getPageSize();\n"),
]

def main(path):
    s = open(path, encoding="utf-8").read()
    for i, (old, new) in enumerate(EDITS, 1):
        n = s.count(old)
        if n != 1:
            sys.exit("patch_sfdp: anchor %d found %d times in %s (expected 1): %r" % (i, n, path, old))
        s = s.replace(old, new)
    open(path, "w", encoding="utf-8").write(s)
    print("patch_sfdp: %d edits applied to %s" % (len(EDITS), path))

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
