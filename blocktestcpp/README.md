# BlockTestCpp

A native C++, VISA-only command-line tool for measuring block-transfer throughput to and from
a test instrument. It is a from-scratch reimplementation of the read/write timing logic in the
`BlockTest` C# WinForms tool, targeting instruments over a raw VISA session instead of a GUI.

## What it does

The tool sweeps a range of block sizes. For each size it:

1. **Pokes** (writes) a block of that many bytes to the instrument and times the round trip.
2. **Peeks** (reads) a block of that many bytes back from the instrument and times the round trip.
3. Repeats for a configurable number of samples per size, tracking min/average/max timing.

Results for every block size are written to a CSV file (always, even if a run fails partway
through), recording the VISA address, sweep parameters, and per-size timing/throughput.

## Wire protocol

Each transfer uses a simple SCPI-like block framing that must match the parser implemented on
the instrument side:

| Operation    | Request sent                                        | Response expected                                    |
|--------------|------------------------------------------------------|--------------------------------------------------------|
| Poke (write) | `W? #<numDigits><length>` + `<length>` bytes + `\n`   | `<length>\n` or `+<length>\n`                          |
| Peek (read)  | `R? <length>\n`                                       | `#<numDigits><length>` + `<length>` bytes of data + `\n` |

`<numDigits>` is the number of decimal digits in `<length>` (standard IEEE‑488.2 definite-length
block header). Payload bytes are a repeating `A`–`Z` pattern.

## Command-line usage

```
BlockTestCpp.exe [options]
  -a <address>   VISA resource string (default TCPIP0::127.0.0.1::hislip0::INSTR)
                 or e.g. TCPIP0::host::5025::SOCKET for a raw-socket VISA session
  -i <bytes>     initial block size (default 500)
  -f <bytes>     final block size (default 130000)
  -c <bytes>     size increment (default 500)
  -m <factor>    size multiplier (default 1)
  -n <count>     samples per block size, used for min/ave/max (default 1)
  -csv <path>    CSV output file, always written (default BlockTest.csv)
  -v             verbose: print each individual write/read sample timing
  -h, -?         show this help
```

Block sizes are generated starting at `-i`, each subsequent size computed as
`size * multiplier + increment`, stopping once the size would exceed `-f`.

### Examples

Sweep the default range against a simulated/loopback instrument:

```
BlockTestCpp.exe
```

Sweep 1 KB to 1 MB in 1 KB steps, 5 samples per size, against a real instrument over raw sockets,
with per-sample verbose output:

```
BlockTestCpp.exe -a TCPIP0::192.168.1.50::5025::SOCKET -i 1024 -f 1048576 -c 1024 -n 5 -v -csv results.csv
```

## Requirements

- Windows, 64-bit.
- A VISA implementation (NI-VISA, Keysight IO Libraries Suite, or other IVI Foundation VISA)
  installed, providing `visa.h`/`visa64.lib` and the runtime `visa64.dll` used to actually open
  and talk to instrument sessions.
- An instrument (real or simulated) that implements the `W?`/`R?` block-transfer commands
  described above.

## Building

64-bit only, with Debug and Release configurations.

**Visual Studio 2026:** open `BlockTestCpp.sln` and build (`Ctrl+Shift+B`). Set the solution
configuration to `Debug` or `Release` and the platform to `x64`.

**Command line:** run `build.bat` from a regular command prompt (no need to launch a Developer
Command Prompt first — it locates `MSBuild.exe` itself via `vswhere`).

```
build.bat            REM builds both Debug and Release, x64
build.bat Debug       REM builds Debug only
build.bat Release     REM builds Release only
build.bat clean       REM removes bin\ and obj\
```

Binaries land in `bin\x64\Debug\BlockTestCpp.exe` and `bin\x64\Release\BlockTestCpp.exe`.

The project locates the VISA SDK via the `VXIPNPPATH64` environment variable, which any 64-bit
VISA installer (NI-VISA, Keysight IO Libraries Suite) sets automatically. No manual configuration
is needed as long as VISA is installed.

## Output

Console output shows progress per block size (average write/read throughput). The CSV file
contains:

- Header rows: VISA address, start time, and the sweep parameters used for the run.
- One data row per block size: size, average write/read throughput (bytes/sec), and min/average/max
  write and read times (seconds).

The tool exits non-zero if the VISA session or any transfer fails; whatever results were
collected before the failure are still written to the CSV.
