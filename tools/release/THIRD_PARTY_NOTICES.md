# Third-party notices: Haunting Ground Recomp release

Haunting Ground Recomp (the launcher `HauntingGround.exe`, the recompiler and the runtime in `engine/src`) is free
software under the GNU General Public License version 3 or later (`LICENSE.txt`). The game program itself is not in
this download: it is built on your PC from your own disc image, and what is built stays on your PC.

**The HD textures** in `textures\` are not ours: they are high-resolution versions of the game's own textures (Capcom's
artwork), made by upscaling them with the AI models 4xHDcube3 and 4x-PBRify_UpscalerV4 and mixing the two
(`engine\src\tools\textures`). They are included so that owners of the game get the best picture without any setup;
the game uses them only when it runs from your own disc. They are not covered by this project's licence. Delete the
`textures` folder (or set "HD textures" to Off in the Options menu) to play with the original textures.

So that you do not have to install anything, the release bundles a C compiler and Python. They are unmodified binaries
from the MSYS2 project (https://www.msys2.org/, UCRT64 environment) and from python.org. Each keeps its own licence; the
licence texts are in `engine/licenses/`, one folder per package.

## Bundled packages

<!-- PACKAGES -->

What each one is for:

- **gcc, libgcc** (GPL-3.0-or-later WITH GCC Runtime Library Exception 3.1): the C compiler (`gcc.exe`, `cc1.exe`,
  `collect2.exe`, `lto-wrapper.exe`, `liblto_plugin.dll`) and its runtime support (`libgcc.a`, `crtbegin.o`, ...). The
  runtime exception lets the program you build be under any licence; ours is GPL-3.0 anyway.
- **binutils** (GPL-3.0-or-later): the assembler and the linker (`as.exe`, `ld.exe`).
- **gmp, mpfr, mpc** (LGPL-3.0-or-later; GMP also GPL-2.0-or-later), **isl** (MIT), **zlib** (Zlib), **zstd**
  (BSD-3-Clause or GPL-2.0-or-later), **gettext-runtime** (`libintl-8.dll`, LGPL-2.1-or-later), **libiconv** (LGPL-2.1-or-later):
  libraries the compiler and the linker load.
- **headers, crt** (MinGW-w64: ZPL-2.1 and public domain; a few headers LGPL-2.1-or-later), **winpthreads,
  libwinpthread** (MIT and BSD-3-Clause-Clear), **windows-default-manifest** (public domain): the Windows headers,
  import libraries and C runtime startup code the game is compiled against; `libwinpthread-1.dll` is copied next to the
  game.
- **SDL2** (Zlib): window, sound, keyboard and controllers; `SDL2.dll` is copied next to the game.
- **vulkan-headers** (Apache-2.0 OR MIT) and **vulkan-loader** (Apache-2.0): the GPU renderer's Vulkan interface;
  `vulkan-1.dll` (the Khronos loader) is copied next to the game, so the game starts even on a PC without a Vulkan
  driver (it then uses the software renderer).

**Python <!-- PYTHON -->** (Python Software Foundation License Version 2; `engine/licenses/python/LICENSE.txt`, which
also lists the licences of the components Python itself includes): runs the recompiler and the build. Only
`python.exe`, its DLL and a subset of its standard library are bundled. The Windows build of Python includes
**Microsoft Visual C++ runtime** files (`vcruntime140.dll`, `vcruntime140_1.dll`), redistributed here under the
conditions in Python's licence ("Microsoft Distributable Code"): they may only be used on Microsoft operating systems,
and their copyright notices must not be altered.

## Source code

The source code of this project is the repository it was released from; the exact commit is in
`engine/src/COMMIT.txt`, and the sources the build uses are in `engine/src`.

The bundled MSYS2 packages are built from the sources and build scripts published by the MSYS2 project:

- source packages: https://repo.msys2.org/mingw/sources/ (file names `mingw-w64-<name>-<version>.src.tar.zst`, with
  the names and versions in the table above);
- build scripts: https://github.com/msys2/MINGW-packages (folder `mingw-w64-<name>`).

For the GPL and LGPL components (gcc, binutils, gmp, mpfr, mpc, gettext-runtime, libiconv, and the LGPL headers),
the corresponding source is offered under the terms of those licences: if it is no longer available at the addresses
above, ask the project's maintainers (see README.txt) and you will be given a copy.

Python's source code: https://www.python.org/downloads/source/ (the version above).

## Trademarks

Haunting Ground is a trademark of Capcom. PlayStation and DualSense are trademarks of Sony Interactive Entertainment.
Vulkan is a trademark of the Khronos Group. This project is not affiliated with or endorsed by any of them.
