# Third-party notices: Haunting Ground Recomp release (Linux)

Haunting Ground Recomp (the launcher `HauntingGround.sh`, the recompiler and the runtime in `engine/src`) is free
software under the GNU General Public License version 3 or later (`LICENSE.txt`). The game program itself is not in
this download: it is built on your PC from your own disc image, and what is built stays on your PC.

**The HD textures** in `textures/` are not ours: they are high-resolution versions of the game's own textures (Capcom's
artwork), made by upscaling them with the AI models 4xHDcube3 and 4x-PBRify_UpscalerV4 and mixing the two
(`engine/src/tools/textures`). They are included so that owners of the game get the best picture without any setup;
the game uses them only when it runs from your own disc. They are not covered by this project's licence. Delete the
`textures` folder (or set "HD textures" to Off in the Options menu) to play with the original textures.

So that you do not have to install a compiler, the release bundles one:

- **A gcc <!-- GCC --> toolchain from Bootlin** (https://toolchains.bootlin.com/, `x86-64--glibc--stable-2024.05-1`,
  built with Buildroot; unmodified, with the C++ compiler, gdb, gcov, locales and documentation left out):
  **gcc** (GPL-3.0-or-later; its runtime libraries under the GCC Runtime Library Exception 3.1, which lets the game be
  under any licence; ours is GPL-3.0 anyway), **binutils** (the assembler and linker; GPL-3.0-or-later, libiberty
  LGPL-2.1-or-later), **gmp, mpfr, mpc** (LGPL-3.0-or-later; GMP also GPL-2.0-or-later), **glibc 2.39** (its headers
  and libraries to compile and link against; LGPL-2.1-or-later, some parts GPL-2.0-or-later, BSD-3-Clause, MIT) and
  the **Linux kernel headers** (GPL-2.0 WITH Linux-syscall-note). The game uses your system's C library when it runs.
  Licence texts: `engine/licenses/gcc-toolchain/` (with Bootlin's README and the component list `summary.csv`).
- **SDL2 headers** (Zlib; `engine/licenses/SDL2-LICENSE.txt`) and **Vulkan headers** (Apache-2.0 OR MIT;
  `engine/licenses/Vulkan-Headers-LICENSE.txt`): the interfaces the game is compiled against. `engine/link` holds
  link stubs made from them (function names only); at run time the game uses your system's SDL2 and Vulkan loader.

Your system needs python3 (3.8 or later), the SDL2 library and, for the GPU renderer, a Vulkan driver.

## Source code

The source code of this project is the repository it was released from; the exact commit is in
`engine/src/COMMIT.txt`, and the sources the build uses are in `engine/src`.

The toolchain's complete source code (gcc, binutils, glibc, gmp, mpfr, mpc, the Linux headers, and Buildroot's
recipes) is published by Bootlin at https://toolchains.bootlin.com/downloads/releases/sources/ (the versions in
`engine/licenses/gcc-toolchain/summary.csv`). For the GPL and LGPL components the corresponding source is also offered
under the terms of those licences: if it is no longer available there, ask the project's maintainers (see README.txt)
and you will be given a copy.

## Trademarks

Haunting Ground is a trademark of Capcom. PlayStation and DualSense are trademarks of Sony Interactive Entertainment.
Vulkan is a trademark of the Khronos Group. This project is not affiliated with or endorsed by any of them.
