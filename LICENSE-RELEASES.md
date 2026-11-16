# Terms of the ready-made files (Releases)

The **source code** of pin16 (this repository: firmware, bootloader project files, web page, tests, documents) is licensed under the GNU General Public License version 3 or later, see `LICENSE`.

The **ready-made files** attached to the Releases (`pin16_vN.ota.bin`, `pin16_vN_diag.ota.bin`, `SOLUM_BTL_FULL_vN*.s37`) contain the pin16 code linked with parts of the Silicon Labs Gecko SDK (Bluetooth stack, Gecko Bootloader, device libraries). They are distributed on these terms, instead of the GPL:

1. **pin16's own code** in the files: MIT licence, text below.
2. **Silicon Labs parts** in the files: the Silicon Labs Master Software License Agreement (https://www.silabs.com/about-us/legal/master-software-license-agreement) and, where a file says so, Zlib or the licence named in it. Use the files only on Silicon Labs devices, as complete applications.

Anyone who wants the code under the GPL, or wants to build the files themselves, takes the source of the same tag from this repository (GitHub attaches it to every release) and builds it with the Dockerfile and the workflow of this repository.

---

MIT License

Copyright (c) 2026 Siarhei Besarab (steanlab)
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

This licence covers the files written for this project (see NOTICE.md). Files of the Silicon Labs Gecko SDK are not part of this repository and keep their own licence.
