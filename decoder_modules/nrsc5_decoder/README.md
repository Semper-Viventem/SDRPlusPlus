# NRSC-5 (HD Radio) decoder

Decodes **HD Radio** (NRSC-5, FM IBOC) by wrapping the GPLv3
[`nrsc5`](https://github.com/theori-io/nrsc5) library. It creates its own VFO locked to
nrsc5's required input rate, feeds the IQ into nrsc5 in *pipe* mode, and plays the selected
digital program (HD1–HD4) while displaying station name/slogan and now-playing metadata.

This module is **off by default** and links against a `libnrsc5` that you install yourself
(same model as the `m17_decoder`/codec2). The `nrsc5` software license is GPLv3 (compatible
with SDR++), but the HDC audio codec it decodes is patent-encumbered — build/use accordingly.

## 1. Build and install libnrsc5

Only `fftw3f` is a hard dependency; the RTL-SDR/FAAD2 pieces are bundled. To install into a
prefix without root, use `~/.local`:

```sh
git clone --recursive https://github.com/theori-io/nrsc5
cd nrsc5 && mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DBUILD_CLI=OFF -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"
make install
```

> **Note:** if the system `librtlsdr` is absent, nrsc5 builds a vendored copy whose static
> archive may be named `librtlsdr.a` and built without `-fPIC`, which breaks the final
> `libnrsc5.so` link (`relocation R_X86_64_32 ... recompile with -fPIC`). The simplest fix is
> to install the system `librtlsdr` dev package first (e.g. `sudo dnf install rtl-sdr-devel`,
> `sudo apt install librtlsdr-dev`) so nrsc5 uses it instead of the vendored one.

## 2. Build the module

```sh
cmake -B build -DOPT_BUILD_NRSC5_DECODER=ON -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build --target nrsc5_decoder -j"$(nproc)"
```

If `libnrsc5` is in a non-standard prefix, point CMake at it with `-DNRSC5_DIR=<prefix>` or
`-DCMAKE_PREFIX_PATH=<prefix>`.

## 3. Use it

1. Enable an instance in **Module Manager** (module type `nrsc5_decoder`).
2. Use an SDR running at **≥ ~1.5 MHz** sample rate. For best performance set the source rate to
   nrsc5's native **1488375 Hz** (or a multiple like 2976750 Hz): the VFO runs at 1488375 Hz, so a
   matching source rate makes the VFO resampling trivial. Other rates (e.g. 2 MHz) work but build a
   larger resampling filter.
3. Tune the VFO onto an HD Radio FM station. When it locks, "Synchronized" shows and MER rises.
4. Pick the program (HD1–HD4). Station text and now-playing info appear as they arrive.

### Why 1488375 Hz?

nrsc5's cs16 FM pipe wants exactly 744187.5 Hz — a *fractional* rate. SDR++'s VFO resampler rounds
its target to an integer, which turns that into a near-coprime ratio and designs a ~19-million-tap
filter (huge memory, slow retunes). The module instead runs the VFO at the integer **1488375 Hz**
(nrsc5's native CU8 rate, exactly 2×) and decimates by 2 with a cheap halfband to reach 744187.5 Hz
exactly. Keep that in mind if you change the rates.

## Status / TODO

- FM only (AM HD would need a second VFO rate, `NRSC5_SAMPLE_RATE_CS16_AM`).
- Album art (LOT/`NRSC5_MIME_PRIMARY_IMAGE`) and a constellation display are not yet wired up.
- Windows/macOS build branches are stubs; only the Linux path is exercised so far.
