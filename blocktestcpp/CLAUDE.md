# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`BlockTestCpp.cpp` is a native C++, VISA-only reimplementation of `Source\mtests\BlockTest`, a C# WinForms
tool (`BlockTest.cs`, not part of this repo). It sweeps write ("poke") and read ("peek") block sizes against
a VISA instrument, times each transfer, and writes the results to a CSV. It is a single-file, single-`main()`
command-line program built via `BlockTestCpp.sln`/`BlockTestCpp.vcxproj` (Visual Studio 2026, x64 only,
Debug/Release) or `build.bat` from the command line. See `README.md` for user-facing docs.

Treat `BlockTestCpp.cpp` as the entire codebase; the `.sln`/`.vcxproj`/`build.bat`/`README.md` are supporting
project files, not application logic.

## Building

Open `BlockTestCpp.sln` in Visual Studio 2026 (x64 platform, Debug or Release config), or run `build.bat`
from a regular command prompt -- it locates `MSBuild.exe` via `vswhere` itself, no Developer Command Prompt
needed. `build.bat clean` removes `bin\`/`obj\`. The project reads the VISA SDK location from the
`VXIPNPPATH64` environment variable (set automatically by the IVI Foundation / Keysight IO Libraries
installer), currently `C:\Program Files\IVI Foundation\VISA\Win64` on this machine; no manual path
configuration should be needed on a machine with VISA installed.

It also builds with gcc/g++ (e.g. MinGW-w64, such as the one bundled with Strawberry Perl, or any
other MinGW-w64 distribution) from a regular shell -- no VISA SDK changes needed, since MinGW's linker
can consume the MSVC-format import libraries under `Lib_x64\msc` directly:

```
g++ -std=c++17 BlockTestCpp.cpp ^
    -I "C:\Program Files\IVI Foundation\VISA\Win64\Include" ^
    -L "C:\Program Files\IVI Foundation\VISA\Win64\Lib_x64\msc" -lvisa64 ^
    -o BlockTestCpp.exe
```

Run it against a simulated/loopback instrument via `-a TCPIP0::127.0.0.1::hislip0::INSTR` (the default), or
point `-a` at a real VISA resource string (e.g. `TCPIP0::host::5025::SOCKET` for a raw-socket connection).
Use `-h` to see all CLI flags (initial/final/increment/multiplier block size, samples per size, CSV path,
verbose per-sample timing).

There is no automated test suite. Validating a change means building and running the tool against a real
or simulated VISA instrument and inspecting the console output / generated CSV.

## Architecture

Everything lives in one file, organized top-to-bottom as: CLI options -> block-size sweep generation ->
VISA transport -> per-size sampling loop -> CSV output -> `main`.

- **Wire protocol** (see the file header comment and `VisaBlockTester`): this tool must byte-for-byte match
  the SCPI-like block-transfer protocol implemented on the instrument side by the original C# tool
  (`BlockTest.cs`, write path ~line 2892-2947, read path ~line 2965-3048). Any change to the framing here
  needs a corresponding correct instrument-side parser, so treat the protocol comments as load-bearing:
  - Poke (write): send `"W? #<numDigits><length>"` + `<length>` bytes of payload + `"\n"`; instrument
    replies `"<length>\n"` or `"+<length>\n"`.
  - Peek (read): send `"R? <length>\n"`; instrument replies `"#<numDigits><length>"` + `<length>` bytes of
    data + `"\n"`.
- **`ComputeBlockSizes`** intentionally replicates the exact loop semantics of `BlockTest.cs:2451-2469`
  (condition-before-body, increment-at-end-of-iteration, and a `lastLength` guard that bumps the size by 1
  when the multiplier/increment combination would otherwise stall on the same value). Don't "clean up" this
  loop without checking it still produces the same sequence as the C# original.
- **`VisaBlockTester`** owns the VISA session (`viOpenDefaultRM`/`viOpen`) and pre-allocates write/read
  buffers sized to `finalSize + kFramingOverhead`. `TimedPoke`/`TimedPeek` each time a single
  `viWrite` + `viRead` pair together as one span (matching `IoTypeVisa.cs`'s `Query()`, which does not time
  write and read separately) and validate the response framing/echoed length before returning.
- **CSV output** (`WriteCsv`) mirrors the column layout of `BlockTest.cs:3387,3473-3477`, with metadata
  rows simplified to plain `Key,Value` pairs. The original tool's CPU/RAM metadata rows (sourced from a
  separate native helper DLL) are deliberately dropped rather than reimplemented.
- The CSV is **always** written on exit, even if the run fails partway through the sweep -- `main` collects
  whatever `SizeStats` were gathered before the exception and writes them regardless of `exitCode`.
