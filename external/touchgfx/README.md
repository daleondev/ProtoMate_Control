# TouchGFX 4.27.0

This directory contains the ST SDK subset used by ProtoMate's STM32H747 M7
application and its Linux development simulator. It is checked into this
repository as ordinary files, including the precompiled libraries and converters.
No separate SDK download is needed for a normal build.

## Contents and provenance

| Included component | Use |
| --- | --- |
| `framework/include` | Public SDK headers, including MVP and rendering interfaces |
| `framework/source` | Supplied widget/container and platform support sources |
| `lib/core/cortex_m7/gcc/libtouchgfx-float-abi-hard.a` | M7 hard-float engine |
| `lib/linux/libtouchgfx.a` | x86-64 Linux simulation engine |
| `framework/tools/textconvert` | Text/font generator, templates and XML schema |
| `framework/tools/{fontconvert,imageconvert}/build/linux/*.out` | x86-64 Linux asset converters |
| `Package_license.html`, `LICENSE` | Original package terms and accompanying project license |

All imported files are byte-identical to the ST archive identified in
[`sdk.json`](sdk.json). `LICENSE` is an unchanged copy of its
`Projects/LICENSE.txt`; the complete package terms, SBOM and third-party annexes
remain in `Package_license.html`. Per-file notices and the canvas renderer's
own `license.txt` are retained. `.gitattributes` prevents line-ending conversion.

Designer, example projects, Windows tools, video tools, unused compiler/CPU
libraries and the bundled Windows SDL distribution are omitted. System tools
(Ruby with nokogiri, SDL2, librsvg and the C++ runtime) are installed separately;
they are not copied from this SDK. No JPEG decoder library is needed by our port.

## Licensing

Copyright STMicroelectronics and the respective third-party authors. TouchGFX
remains proprietary under **SLA0048**, with separate terms for bundled third-party
components. It is not relicensed under this project's or any dependency's license.

The package's SLA0048 permits source and binary redistribution, including selected
parts, subject to its conditions. Retain the supplied copyright/license notices,
conditions, disclaimer and applicable third-party notices when redistributing
this directory or its binaries. Use is restricted to execution on or in
combination with ST devices; our Linux build is the development simulator for
the STM32 application, not a separately licensed general-purpose desktop product.
Do not impose an incompatible license such as GPL on the ST components. The
package's export/redistribution conditions also continue to apply. See the
original documents for the complete terms, rather than treating this summary
as a replacement license.

The package SBOM identifies framework components including Penner easing
equations (BSD-3-Clause/MIT), Nayuki QR Code (MIT), QOI (MIT), and Nema.GFX
(Annex 5). The retained canvas renderer carries its own Anti-Grain Geometry
notice. Nema GPU acceleration is not used on our H747. Converter dependencies
listed by ST include FreeType (Annex 4), libpng (Annex 1), zlib, Nano SVG and
TinyXML (Zlib), and nlohmann_json (MIT); their full notices are preserved in the
package document. The converters are **based in part on the work of the FreeType
Team** (https://freetype.org/).

ST also confirms the source-sharing conditions in its
[TouchGFX licensing answer](https://community.st.com/stm32-mcus-touchgfx-and-gui-33/touchgfx-licenses-156993).
The license bundled with this exact 4.27.0 archive is the retained reference.

## Verification and reimport

From this directory, `sha256sum --check SHA256SUMS` verifies every imported file.
The source archive is deliberately not versioned. To reproduce the subset from
the original downloaded archive, run from the repository root:

```sh
python3 scripts/touchgfx_setup.py apps/STMicroelectronics.X-CUBE-TOUCHGFX.4.27.0.zip
```

The script verifies the pinned archive SHA256 before writing anything, selects
the files listed in `sdk.json`, retains the licenses and generates `SHA256SUMS`.
To inspect a future SDK update without overwriting this directory, use `--output`
with a separate destination and review its license and build compatibility first.
