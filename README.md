# pdn_aprs: APRS in C

An APRS encoder and decoder in C99. It reads TNC2 / APRS-IS lines and AX.25 UI frames, tells you what each packet means and what is wrong with it, and writes packets back out. It is two files, `pdn_aprs.h` and `pdn_aprs.c`, plus an optional third holding the device identification table, and it is meant to be dropped into an existing C program such as a packet node, an IGate or a digipeater.

It is the C member of a family of APRS libraries that all run the same language-neutral conformance vectors, [packet-net/aprs-vectors](https://github.com/packet-net/aprs-vectors): [Packet.Aprs](https://github.com/packet-net/packet.net) (C#), [pdn-aprs](https://github.com/packet-net/aprs-rs) (Rust), [pdn-aprs](https://github.com/packet-net/aprs-py) (Python) and [@packet-net/pdn-aprs](https://github.com/packet-net/aprs-ts) (TypeScript). It passes every check in the vectors, and at 0.1.0 it decoded all 6,879,893 packets of a real APRS-IS capture exactly as the Rust library did.

- Every APRS data type: positions (plain, compressed, with timestamps, ambiguity, `!DAO!`, data extensions, frequencies, base-91 telemetry), Mic-E, objects, items, weather, messages, acks, bulletins, NWS bulletins, telemetry and its metadata, status, queries, capabilities, third-party, NMEA, raw weather, user-defined, test and Agrelo DF.
- Lenient decoding by default, strict on request, and each tolerated defect switchable on its own.
- An encoder that writes only what the spec allows and refuses the rest, and builder calls for the packets a station sends.
- No dynamic allocation, no global state, nothing locale-dependent. Every input is a pointer and a length.

## Contents

- [Getting it](#getting-it)
- [Decoding](#decoding)
- [Strict and lenient parsing](#strict-and-lenient-parsing)
- [Building a packet](#building-a-packet)
- [AX.25 frames](#ax25-frames)
- [Embedding it in a daemon](#embedding-it-in-a-daemon)
- [Conformance](#conformance)
- [Building from source](#building-from-source)
- [Licence](#licence)

## Getting it

Each [release](https://github.com/packet-net/aprs-c/releases) has a `.tar.gz` and a `.zip` holding the three source files, the licence and notices, this README and the changelog. The same files are in [`dist/`](dist) on `main`:

| File | What |
|---|---|
| `pdn_aprs.h` | The whole public API. |
| `pdn_aprs.c` | The whole library. |
| `pdn_aprs_deviceid.c` | Optional: the device identification table from [aprs-deviceid](https://github.com/aprsorg/aprs-deviceid), CC BY-SA 2.0. |

Add them to your build; there is nothing to configure. CMake users can also build and install the library and use `find_package(pdn_aprs)` or `pkg-config pdn_aprs` ([Building from source](#building-from-source)).

## Decoding

Decode a line into a `pdn_aprs_packet`. The packet holds the header, the raw information field, the decoded data and a list of diagnostics.

<!-- example: examples/decode.c -->
```c
/* Decodes a TNC2 line and prints what it says. */
#include <stdio.h>
#include <string.h>

#include "pdn_aprs.h"

int main(void)
{
    static pdn_aprs_packet p; /* about 2.5 KB: static here, or on a roomy stack */
    const char *line = "M0LTE-9>APZ001,WIDE1-1,qAR,G4XYZ-10:=5130.00N/00007.00W>088/036Mobile";
    int i;

    if (pdn_aprs_decode_tnc2(line, strlen(line), NULL, &p) != PDN_APRS_OK) {
        printf("unusable header\n");
        return 1;
    }
    printf("from %s, a %s report\n", p.header.source, pdn_aprs_type_name(p.data.type));
    if (p.data.type == PDN_APRS_TYPE_POSITION) {
        const pdn_aprs_report *r = &p.data.as.report;
        printf("at %.4f, %.4f: %s\n", r->latitude, r->longitude, pdn_aprs_symbol_description(r->symbol));
        if (r->has_course && r->has_speed)
            printf("heading %u degrees at %.0f knots\n", (unsigned)r->course_degrees, r->speed_knots);
        printf("comment: %s\n", r->comment);
    }
    for (i = 0; i < p.diagnostic_count; i++)
        printf("%s: %s\n", pdn_aprs_severity_name(p.diagnostics[i].severity),
               pdn_aprs_code_name(p.diagnostics[i].code));
    return 0;
}
```

It prints:

```
from M0LTE-9, a position report
at 51.5000, -0.1167: Car
heading 88 degrees at 36 knots
comment: Mobile
```

The shape of the API:

- `pdn_aprs_decode_tnc2()`, `pdn_aprs_decode_ax25()` and `pdn_aprs_decode_info()` fill a `pdn_aprs_packet`. They return `PDN_APRS_OK`, or `PDN_APRS_ERR_HEADER` when the header is unusable (the packet's diagnostics still say why), or `PDN_APRS_ERR_TOO_LONG` for an information field over 512 bytes, which nothing valid has.
- `packet.header` has the source, destination, path (each entry with its used flag) and the index of any APRS-IS q-construct.
- `packet.data.type` says which member of the `packet.data.as` union to read. Positions, Mic-E, objects and items share `as.report`; messages, bulletins and NWS bulletins `as.message`; the telemetry metadata types `as.meta`; and so on, one struct per kind.
- Optional fields have a `has_` flag, or are an empty string. Numbers are in the units APRS sends (`speed_knots`, `altitude_feet`, `temperature_f`), as the vectors' neutral form names them.
- Text is UTF-8 in bounded, NUL-terminated arrays with an explicit length (text can hold a NUL). Text that was not valid UTF-8 is read as Latin-1, converted, and flagged with a `non-utf8-text` warning. The packet keeps no pointer into your input, so you can reuse the input buffer and copy the packet.
- `packet.diagnostics` lists each diagnostic as a severity (info, warning, error) and a code. `pdn_aprs_code_name()` gives the vectors' name for a code, such as `trailing-line-break`.
- A third-party packet (`}`) keeps the encapsulated packet as received; `pdn_aprs_decode_third_party()` decodes it into a packet of its own.
- `pdn_aprs_identify_device()` names the sending radio or program, from a Mic-E report's type code and suffix or from the destination address.

## Strict and lenient parsing

Real traffic is full of small defects: a trailing carriage return, lower-case hemisphere letters, an object name that is not padded to nine characters, a Kenwood radio's `0xFF` padding. The vectors list 32 of them as tolerable. A lenient decoder, the default, accepts each with a warning and decodes the packet anyway. A strict decoder rejects the packet at the first one, which leaves `data.type` as unrecognized with reason malformed and an error diagnostic naming the defect; a defect in the header rejects the header instead.

```c
pdn_aprs_decode_options lenient = {0};                     /* the default; NULL does the same */
pdn_aprs_decode_options strict = {.strict = 1};
pdn_aprs_decode_options fussy = {.reject = {[PDN_APRS_CODE_TRAILING_LINE_BREAK] = 1}};
```

`reject` switches off one tolerance at a time, indexed by the code; `pdn_aprs_code_tolerable()` says which codes can be tolerated. A few tolerances change how a packet is read rather than rejecting it: with `position-not-at-start` switched off, text before a `!` position makes the packet a non-APRS beacon, and with `data-extension-in-comment` or `mic-e-altitude-not-first` switched off the element stays in the comment.

The options also carry the device table, which a decoder needs to recognise Mic-E device suffixes:

```c
pdn_aprs_decode_options options = {.devices = pdn_aprs_devices()}; /* from pdn_aprs_deviceid.c */
```

## Building a packet

A `pdn_aprs_station` names the sender (the destination defaults to `APZ001`, the experimental-software tocall) and each `pdn_aprs_build_*` call writes one kind of packet as a TNC2 line, NUL-terminated, or as an AX.25 frame. There is a call for positions, objects, items, Mic-E, weather, messages, acks and rejects, bulletins, status, telemetry, and the telemetry names, units, coefficients and bit sense. Each returns the number of bytes written, or a negative result.

<!-- example: examples/build.c -->
```c
/* Builds the packets a station sends, as TNC2 lines. */
#include <stdio.h>

#include "pdn_aprs.h"

int main(void)
{
    char line[600];
    int n;
    pdn_aprs_station me = {.source = "M0LTE-9", .path = "WIDE1-1,WIDE2-1"}; /* destination: APZ001 */
    pdn_aprs_position pos = {.latitude = 51.5,
                             .longitude = -0.1167,
                             .symbol = PDN_APRS_SYMBOL_CAR,
                             .has_course = 1,
                             .course_degrees = 88,
                             .has_speed = 1,
                             .speed_knots = 36,
                             .comment = "Mobile"};
    double analog[5] = {199, 0, 255, 73, 123};

    n = pdn_aprs_build_position(&me, &pos, NULL, 1, line, sizeof line);
    if (n < 0) {
        printf("refused: %s\n", pdn_aprs_strerror(n));
        return 1;
    }
    printf("%s\n", line);

    n = pdn_aprs_build_message(&me, "G4XYZ", "Meet at the club at 8?", "42", line, sizeof line);
    printf("%s\n", n >= 0 ? line : pdn_aprs_strerror(n));

    n = pdn_aprs_build_telemetry(&me, "005", analog, "01101001", NULL, line, sizeof line);
    printf("%s\n", n >= 0 ? line : pdn_aprs_strerror(n));

    /* A message over 67 characters is refused, not truncated. */
    n = pdn_aprs_build_message(&me, "G4XYZ",
                               "This text is far too long for an APRS message, which carries 67 characters.",
                               NULL, line, sizeof line);
    printf("long message: %s\n", pdn_aprs_strerror(n));
    return 0;
}
```

It prints:

```
M0LTE-9>APZ001,WIDE1-1,WIDE2-1:=5130.00N/00007.00W>088/036Mobile
M0LTE-9>APZ001,WIDE1-1,WIDE2-1::G4XYZ    :Meet at the club at 8?{42
M0LTE-9>APZ001,WIDE1-1,WIDE2-1:T#005,199,0,255,73,123,01101001
long message: the encoder refuses this data
```

The encoder writes only what APRS allows. It refuses, with `PDN_APRS_ERR_REFUSED`, data it cannot write faithfully: message text over 67 characters, a comment that would read back as an altitude or a `!DAO!`, two data extensions, an object without a timestamp, a weather report with a comment, and so on. When it refuses, `pdn_aprs_encoded.reason` says why in plain words. As a last check it decodes what it has written and refuses if that does not read back as the same data, so what it writes always decodes cleanly. Numbers are rounded to what the format carries: a compressed position to its base-91 step, an uncompressed one to a hundredth of a minute unless a `!DAO!` asks for more.

For full control, fill a `pdn_aprs_data` yourself (or take one from the decoder) and call `pdn_aprs_encode_info()`, `pdn_aprs_encode_tnc2()` or `pdn_aprs_encode_ax25()`. A Mic-E report's destination address is computed from its latitude and message type; the encoder returns it in `pdn_aprs_encoded.destination`. Pass the device table in `pdn_aprs_encode_options` (or a station's `devices`) when you have it: the encoder then also refuses a Mic-E comment that happens to end in a known device suffix, which would read back as the suffix.

Every symbol in the APRS symbol tables has a name, `PDN_APRS_SYMBOL_CAR`, `PDN_APRS_SYMBOL_WEATHER_STATION` and so on, the same names the other packet-net libraries use. `pdn_aprs_symbol_overlay()` puts an overlay character on an alternate-table symbol, and `pdn_aprs_symbol_description()` gives a symbol's description.

## AX.25 frames

`pdn_aprs_decode_ax25()` takes an AX.25 UI frame as KISS carries it: addresses, control, PID and information field, without flags or FCS. With `ax25 = 1` in the station, the build calls write a frame in the same form, ready to wrap in KISS.

<!-- example: examples/ax25.c -->
```c
/* Decodes an AX.25 UI frame strictly and leniently, then builds one. */
#include <stdio.h>

#include "pdn_aprs.h"

int main(void)
{
    /* KISS payload (no flags, no FCS): K2CAT-1 to APAT51, padded with NUL
       rather than spaces, which a lenient decoder tolerates. */
    static const unsigned char frame[] = {
        0x82, 0xA0, 0x82, 0xA8, 0x6A, 0x62, 0xE0, 0x96, 0x64, 0x86, 0x82, 0xA8, 0x00, 0x63, 0x03, 0xF0,
        '!',  '4',  '1',  '5',  '0',  '.',  '6',  '7',  'N',  '/',  '0',  '7',  '4',  '0',  '4',  '.',
        '7',  '1',  'W',  '-'};
    static pdn_aprs_packet p;
    pdn_aprs_decode_options strict = {.strict = 1};
    pdn_aprs_decode_options fussy = {.reject = {[PDN_APRS_CODE_NUL_PADDED_ADDRESS] = 1}};
    unsigned char out[330];
    pdn_aprs_station me = {.source = "M0LTE-7", .path = "WIDE2-1", .ax25 = 1};
    int n, i;

    pdn_aprs_decode_ax25(frame, sizeof frame, NULL, &p);
    printf("lenient: %s from %s, %d diagnostic(s)\n", pdn_aprs_type_name(p.data.type), p.header.source,
           p.diagnostic_count);

    n = pdn_aprs_decode_ax25(frame, sizeof frame, &strict, &p);
    printf("strict: %s\n", n == PDN_APRS_ERR_HEADER ? "header rejected" : "accepted");

    /* Lenient, except that NUL padding is rejected. */
    n = pdn_aprs_decode_ax25(frame, sizeof frame, &fussy, &p);
    printf("rejecting only NUL padding: %s\n", n == PDN_APRS_ERR_HEADER ? "header rejected" : "accepted");

    n = pdn_aprs_build_status(&me, "On the air", NULL, out, sizeof out);
    printf("built a %d-byte frame:", n);
    for (i = 0; i < n; i++)
        printf(" %02X", out[i]);
    printf("\n");
    return 0;
}
```

## Embedding it in a daemon

The library was written to be added to the source tree of a program like LinBPQ/BPQ32 or XRouter.

**What to compile.** Add `pdn_aprs.c` and, if you want device identification, `pdn_aprs_deviceid.c`. They need a C99 compiler, or MSVC 2015 or later; they build without warnings under `-Wall -Wextra -Wpedantic -Wconversion` and MSVC `/W4`, on 32- and 64-bit targets. Link the maths library (`-lm`) on Unix. There are no configuration macros to set. `pdn_aprs.h` also compiles as C++.

**Names.** Every public function, type and macro starts `pdn_aprs_` or `PDN_APRS_`, so nothing collides with a program's own APRS code. Internal functions are `static`, and the internal macros are undefined at the end of `pdn_aprs.c`, so it can even be `#include`d into another file.

**Memory.** Nothing is allocated. You own every struct and buffer. A `pdn_aprs_packet` is about 3.4 KB and a `pdn_aprs_data` about 2.6 KB; keep them static, in your own structures, or on a stack with room. Measured over the vectors and a fuzzing corpus on x86-64, decoding uses up to about 13 KB of stack, and encoding or building up to about 20 KB, most of it the decode the encoder does to check its own output. Outputs never go past the capacity you pass; an information field over 512 bytes is refused before anything is copied.

**Threads.** There is no global mutable state. Every function is reentrant and can run on any number of threads at once, as long as each call has its own packet and buffers. The device table is read-only.

**Locale.** Nothing depends on the C locale: the library parses and prints numbers itself, so a program running under a German locale still reads `4903.50N` and writes `0.53`.

**The device table.** `pdn_aprs_deviceid.c` holds the [aprs-deviceid](https://github.com/aprsorg/aprs-deviceid) database, which is licensed CC BY-SA 2.0 rather than MIT. It is optional: without it the library builds and works, `pdn_aprs_identify_device()` reports the device as unknown, and Mic-E device suffixes such as `_%` (a Yaesu FTM-400) stay at the end of the comment instead of being lifted out. A closed-source product can leave the file out, or supply its own table in the same form (`pdn_aprs_device_table`, documented in the header). If you ship it, carry the attribution in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). To refresh it, run `tools/gen_deviceid.py` on a checkout of aprs-deviceid.

**Where it fits.** A node that gates or digipeats APRS usually wants three things: to decode what it hears (`pdn_aprs_decode_ax25`) and what arrives from APRS-IS (`pdn_aprs_decode_tnc2`), to judge whether a packet is sound (the diagnostics, or a strict decode), and to build its own beacons, status, messages and acks (the build calls). None of it needs any setup.

## Conformance

The tests run every check the vectors' README defines against every case in [packet-net/aprs-vectors](https://github.com/packet-net/aprs-vectors), which is a submodule at `vectors/`: the lenient decode, the strict decode, the decode with each single tolerance switched off, the re-encode (identical, equivalent or refused) and the encode cases.

- **Vectors:** 5,999 of 5,999 checks pass over 1,868 cases: 1,806 lenient decodes, 1,806 strict decodes, 690 decodes with a single tolerance switched off, 1,635 re-encodes and 62 encode cases. Nothing is skipped ([`tests/known-differences.txt`](tests/known-differences.txt) is empty). `pdn_aprs_vectors vectors -v` lists every check by case id.
- **A real capture:** at 0.1.0, on 6,879,893 packets from APRS-IS, the decodings (lenient and strict) and re-encodings agreed with the Rust library's on every packet, by the vectors' `tools/compare.py`. The only differences were encoder choices the comparison does not count: for 114 objects this library wrote the original bytes back where Rust wrote different but equivalent ones.
- **Differential fuzzing:** the five implementations were also compared over 2,000,000 mutated packets (2026-09-27). Every disagreement was settled by a ruling, and the rulings are in the vectors as cases.

`tools/diffdump.c` writes the comparison dump: `zcat lines.hex.gz | pdn_aprs_diffdump | gzip > c.jsonl.gz`.

The decoder is also fuzzed with libFuzzer, AddressSanitizer and UndefinedBehaviorSanitizer (`fuzz/fuzz_decode.c`): every input is decoded as a TNC2 line, an AX.25 frame and an information field, leniently and strictly, and whatever decodes is re-encoded and decoded again to check it reads back the same. Inputs that once failed are kept in `fuzz/regressions/`, and the tests replay them on every platform.

## Building from source

The library is developed in several files under `src/`; `tools/amalgamate.py` joins them into `dist/pdn_aprs.c`, and CI checks that `dist/` is current.

```sh
git clone --recurse-submodules https://github.com/packet-net/aprs-c
cd aprs-c
cmake -S . -B build
cmake --build build
ctest --test-dir build
cmake --install build --prefix /usr/local
```

Options: `PDN_APRS_DEVICEID` (on) includes the device table; `PDN_APRS_AMALGAMATION` builds from `dist/` instead of `src/`; `PDN_APRS_WERROR` treats warnings as errors; `PDN_APRS_CXX_CHECK` also compiles the header as C++; `PDN_APRS_BUILD_FUZZ` builds the libFuzzer target with clang. The install gives a CMake package (`find_package(pdn_aprs)`, target `pdn_aprs::pdn_aprs`) and a pkg-config file (`pdn_aprs.pc`).

To fuzz:

```sh
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang -DPDN_APRS_BUILD_FUZZ=ON
cmake --build build-fuzz --target pdn_aprs_fuzz
python3 tools/fuzz_seeds.py vectors corpus
build-fuzz/pdn_aprs_fuzz corpus
```

## Licence

MIT: see [LICENSE](LICENSE). The optional device table in `pdn_aprs_deviceid.c` is data from aprs-deviceid under CC BY-SA 2.0: see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). The conformance vectors in the `vectors/` submodule are AGPL-3.0-or-later; they are only used by the tests and are not part of the library.
