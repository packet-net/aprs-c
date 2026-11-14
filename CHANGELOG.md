# Changelog

## Unreleased

The round 3 rulings of the shared vectors (aprs-vectors `round-3`, pinned at 10d4e25).

### Behaviour changes for 1.x callers

- A lower-case or 5-character locator (status, Mic-E, Maidenhead beacon) is now refused as `malformed`; the status builder still upper-cases one for you.
- AX.25 frames: punctuation, control characters or a space inside an address are now `invalid-address` (an error), where they were the tolerated `invalid-ax25-address-characters`. Lower case is still tolerated.
- Telemetry: more than five analog values is `too-long`, fewer `malformed` (`T#MIC` with none too), where some of these were other codes.
- Third-party packets are read at most four deep; a fifth `}` is unrecognized with `invalid-third-party`.
- When data has several faults the refusal code may differ from 1.0.x: it is now the first in the README's order.

### Changes


- Refusal reasons follow the vectors' order whichever check runs first. The encoder now goes on past a fault in a field on its own, writing bytes it throws away, and names the code that comes first in the README's Refusal reasons order. So a PHG digit of 10 beside a course is `value-out-of-range`, not `extension-conflict`; a Mic-E course of 0 with an altitude past every form is `value-out-of-range`; a query with a bad type and a radius past 9999 miles is `value-out-of-range`; a comment with a line break beside a bad NMEA checksum is `invalid-character`.
- A field whose place is wanted twice keeps its own checks: the PHG, RNG, DFS, area, DF bearing, storm data and course/speed beside an uncompressed weather report's wind, a course or speed beside compressed wind or a GGA altitude (judged by the cs bytes, so a course of 0 there is `value-has-no-form`), and a compressed wind with only one half given.
- More analog values than a telemetry report or base-91 telemetry holds is `too-long`, fewer is `malformed` (`T#MIC` with no values included; an empty first channel is still `value-has-no-form`). Base-91 telemetry with an `analog_count` over `PDN_APRS_MAX_ANALOG` is refused as `too-long`, as a telemetry report's already was.
- A locator must be in the fixed form a decoder reads back: 4 or 6 characters, upper case. A lower-case or 5-character locator in a status, a Mic-E report or a Maidenhead beacon is `malformed`. A status locator with no symbol is `missing-field`.
- A status beam heading code outside 0-9 and A-Z, or power code outside 1-9, `:`-`@` and A-K, is `invalid-code`.
- A query with no type is `invalid-code` (it was `missing-field`), for general and directed queries.
- Telemetry metadata builders leave too many names, units or coefficients, and a project title over 23 characters, to the encoder, so a bad addressee is named first.
- A positionless weather report's comment no longer hides a fault in its fields.
- Mic-E data with no speed is written with a speed of 0, which a decoder reads back as 0 (an ambiguous Mic-E position a rounding step under 90 or 180 degrees is now written).
- A Mic-E report's source and path are checked before its data, as any other report's are (only its destination is computed), so a bad source is `invalid-address` whatever else is wrong.
- Builders: telemetry names, units, bits and a project title the data cannot hold (a 14th name, a name past the buffer, an over-long title or bits) are noted as `too-long` or `malformed` and ranked with what the encoder finds, so a bad addressee or a comma in a 16th name is named first.
- No float-to-integer cast overflows on huge or infinite values (a Mic-E speed or course, a GGA altitude).
- Third-party data nested more than four deep is refused as `invalid-third-party-header` by the neutral conversion, which counts the levels by walking before it converts any. The C data holds a third-party packet as its raw text, so a chain in it is the received form of four levels whose innermost data is unrecognized, and it is written back as received; the decoder reads it one level at a time and stops at the fifth.
- A compressed GGA weather report's altitude of 1 foot or less, however far below, is written as `!!` (1 foot) and re-encodes rounded; -99999 feet was refused.
- A GGA altitude beside wind on a compressed report with a non-weather symbol is `extension-conflict`, named before field-not-allowed and symbol-mismatch.
- Tests: the neutral conversion checks a third-party header's addresses (source, destination and path) before the inner data, so a bad header is `invalid-third-party-header` whatever is wrong inside. The dump tool builds a recipe that gives one quantity in two units once with each unit and refuses it with the first code among both results and `value-given-twice` (README Builder rules), instead of building from one unit.
- Decoder: third-party packets nest at most four deep. `pdn_aprs_third_party` gains `uint8_t depth` at the end, set by the decoder, and `pdn_aprs_decode_third_party()` reads a fifth `}` as unrecognized with `invalid-third-party`. New `PDN_APRS_MAX_THIRD_PARTY_DEPTH` (4).
- Decoder: an AX.25 address is upper-case letters and digits padded with spaces. A lower-case letter stays tolerated (`invalid-ax25-address-characters`); any other character, a space before the last character included, is now `invalid-address`. A call of spaces only is empty: a destination is `empty-destination` and a digipeater `empty-path-entry` (left out of the path), both tolerated.
- Tests: the vectors runner compares refusal codes (`refusal`, `reencode_refusal`) and reads `precedence.json`. The neutral converter refuses a third-party path entry before a used one that is not marked used as `invalid-third-party-header`, before the inner data, and lets the encoder's own checks run beside a fault the C data cannot carry (a message ID on an ack). `third-party/nested-2000-deep` is a known difference: its line is 10,017 bytes, past the 512 the decoder takes.

## 1.0.1 (2026-11-12)

Refusals carry a reason code.

- A refusal now says why in a form a program can act on: `pdn_aprs_encoded` gains `int refusal` at the end (a `pdn_aprs_refusal` code, zero for none and for every error that is not `PDN_APRS_ERR_REFUSED`), and `pdn_aprs_refusal_name()` gives the vectors' kebab-case id. The 26 codes are the refusal ids of the shared vectors; new ones may be appended in a later 1.x release, so keep a default case. Build calls fill it in `result` too. The message text is unchanged and still not stable API.
- Fixed: a header address the encoder cannot write (`pdn_aprs_encode_tnc2` and `pdn_aprs_encode_ax25`) returned `PDN_APRS_ERR_ARGUMENT`, though the header documents `PDN_APRS_ERR_REFUSED`. It is now `PDN_APRS_ERR_REFUSED` with `PDN_APRS_REFUSAL_INVALID_ADDRESS` (TNC2) or `PDN_APRS_REFUSAL_AX25_CANNOT_CARRY` (AX.25) and a reason. NULL pointers stay `PDN_APRS_ERR_ARGUMENT`.
- An altitude given to `pdn_aprs_build_mic_e` in feet and in metres is now `PDN_APRS_ERR_REFUSED` with `PDN_APRS_REFUSAL_VALUE_GIVEN_TWICE`, where it was `PDN_APRS_ERR_ARGUMENT`.
- When data breaks several rules the refusal names the first by the shared order of the vectors' DESIGN.md (a fault in a field on its own before one about how fields relate; the header before the data).
- An address written with SSID `-0` (`N0ZO-0`) is now refused by `pdn_aprs_encode_ax25()` and `pdn_aprs_write_ax25()` with `PDN_APRS_REFUSAL_AX25_CANNOT_CARRY`, like `WB4APR-00`, since a frame gives it back as `N0ZO`. This holds for a third-party inner header too. It was written with no SSID.
- The dump tool writes `refusal` (encode and build), `reencode_refusal` (decode) and `ax25_refusal` (the API view) in its records.

## 1.0.0 (2026-11-10)

This is the first stable release. The public API is now frozen under semantic versioning. From 1.0 the major and minor version are shared with aprs-vectors and the other four libraries: a ruling round is a minor version for all five, this library's own fixes are patch versions, and an API break is a major version.

The 1.0 API cleanup: one consistent shape for every build call, one way to report a refusal, and a written compatibility promise. Breaking changes are listed first, with what to do about each.

### Breaking changes

What 1.x promises, which is new: source compatibility only. Struct sizes follow the capacities and a 1.x release may add a field to any struct, so rebuild everything that includes `pdn_aprs.h` on upgrade, link the static library or compile in the single file, and zero-initialise every input struct (a field added later means "absent" when zero).

- The CMake library target is always STATIC (`BUILD_SHARED_LIBS` is ignored) and no longer has a SOVERSION. It is built with hidden visibility, and the public functions are marked `PDN_APRS_API`.
- `pdn_aprs_decode_options.reject` is now a fixed `reject[PDN_APRS_MAX_CODES]` (128) instead of `reject[PDN_APRS_CODE_COUNT]`, so a new code never changes the struct. Designated initializers (`.reject = {[PDN_APRS_CODE_X] = 1}`) are unaffected.
- Every `pdn_aprs_build_*` call, and `pdn_aprs_build()`, gains a trailing `pdn_aprs_encoded *result` (may be NULL): it gets `len`, the Mic-E `destination` and, on failure, `reason` (plain text, not part of the stable API). Add `, NULL` to keep today's behaviour.
- One input struct for every build call with optional inputs, with `name`, `timestamp`, `killed`, `messaging` inside the struct that uses them:
  - `pdn_aprs_position` is now `pdn_aprs_position_args` and gains `name`, `killed`, `timestamp` and `messaging`. `pdn_aprs_build_position(station, &args, buf, cap, result)`, `pdn_aprs_build_object(station, &args, buf, cap, result)` and `pdn_aprs_build_item(station, &args, buf, cap, result)` lose their `timestamp`, `messaging`, `name` and `killed` parameters.
  - `pdn_aprs_mic_e` is now `pdn_aprs_mic_e_args`; its `message` is now `mic_e_message`.
  - `pdn_aprs_status_options` is now `pdn_aprs_status_args`, and `pdn_aprs_build_status_options()` is now `pdn_aprs_build_status(station, &args, buf, cap, result)`. The old text-and-timestamp `pdn_aprs_build_status()` is gone (set `.text` and `.timestamp`).
  - New `pdn_aprs_message_args { addressee, text, message_id, reply_ack }`: `pdn_aprs_build_message_reply_ack()` is now `pdn_aprs_build_message(station, &args, buf, cap, result)`. The old four-string `pdn_aprs_build_message()` is gone (leave `reply_ack` NULL).
  - New `pdn_aprs_telemetry_args { sequence, analog[5], empty[5], bits, comment }`: `pdn_aprs_build_telemetry_channels()` is now `pdn_aprs_build_telemetry(station, &args, buf, cap, result)`. `empty[i]` set leaves channel i empty (zero means present, so a zeroed struct has all five); the old five-value `pdn_aprs_build_telemetry()` is gone. `bits` NULL still gives `00000000`.
- Builder inputs now match the recipe: the telemetry `sequence` is an `unsigned` number 0-999 (refused above, written with three digits), where it was text; the bulletin takes `char id` and `const char *group` (NULL for none) where it took one string like `"4WX"`; Mic-E takes a `messaging` flag, and `type_code` is only for a device type (`'>'`, `']'` or `' '`), so `type_code = '`'` or `'\''` is now refused and a Mic-E report is always written with a type code; telemetry metadata calls (`_names`, `_units`, `_coefficients`, `_bits`) accept a NULL addressee, meaning the station itself.
- Builder error codes now depend on the input, not on the C buffer: `PDN_APRS_ERR_REFUSED` for anything APRS cannot carry (a name that is too long, a bad station address), `PDN_APRS_ERR_ARGUMENT` only for a NULL required pointer or other misuse (altitude in feet and metres both), and the new `PDN_APRS_ERR_UNSUPPORTED` (-6) for text APRS allows that does not fit the fixed buffers (over 1,024 bytes). A switch over the result codes needs a case for -6.
- Weather indexes carry their units, and the enum has a type name, `pdn_aprs_weather_field`: `PDN_APRS_WX_WIND_DIRECTION` is `PDN_APRS_WX_WIND_DIRECTION_DEGREES`, `_WIND_SPEED` `_WIND_SPEED_MPH`, `_WIND_GUST` `_WIND_GUST_MPH`, `_TEMPERATURE` `_TEMPERATURE_F`, `_RAIN_1H` `_RAIN_1H_IN`, `_RAIN_24H` `_RAIN_24H_IN`, `_RAIN_MIDNIGHT` `_RAIN_MIDNIGHT_IN`, `_HUMIDITY` `_HUMIDITY_PERCENT`, `_PRESSURE` `_PRESSURE_MBAR`, `_LUMINOSITY` `_LUMINOSITY_W_M2`, `_SNOW_24H` `_SNOW_24H_IN`. `PDN_APRS_WX_RAIN_RAW` is unchanged.
- The `#define` groups are now named enums (the fields stay `uint8_t`): `pdn_aprs_fix`, `pdn_aprs_source`, `pdn_aprs_origin`, `pdn_aprs_area_shape`, `pdn_aprs_storm_type`, `pdn_aprs_dao_precision`, `pdn_aprs_tone`, `pdn_aprs_raw_weather_format`. `PDN_APRS_NMEA_OTHER`, `_GLL`, `_GGA` and `_RMC` (the compression source) are now `PDN_APRS_SOURCE_*`.
- Renames: `pdn_aprs_status.beam_heading` and `beam_power` are `beam_heading_code` and `beam_power_code` (and the same in `pdn_aprs_status_args`); `pdn_aprs_reason` is `pdn_aprs_unrecognized_reason` and `PDN_APRS_REASON_*` is `PDN_APRS_UNRECOGNIZED_*`; `pdn_aprs_meta_item()` is `pdn_aprs_telemetry_meta_item()`; `pdn_aprs_decode_written()` is `pdn_aprs_decode_info_unbounded()`.
- `pdn_aprs_options_strict()` is removed: write `pdn_aprs_decode_options o = {.strict = 1};` or set `o.strict = 1`.
- `pdn_aprs_capabilities.item` is an array of the named struct `pdn_aprs_capability` (it was an anonymous struct); same fields.
- `pdn_aprs_ack.id` keeps its name (the neutral form calls it `acked_id` or `rejected_id`, and `message_id` would mean the ack's own ID elsewhere in the family).
- `pdn_aprs_report` gains `altitude_in_comment` (see Mic-E below), so struct layouts change.

### Added

- `pdn_aprs_format_timestamp(year, month, day, hour, minute, second, format, out)` writes a UTC time as `DDHHMMz`, `HHMMSSh` or `MMDDHHMM` for the builders, cutting to the format's step.
- `pdn_aprs_write_tnc2()` and `pdn_aprs_write_ax25()` write a decoded packet back as received (header and information field), for an IGate or digipeater. They return `PDN_APRS_ERR_HEADER` for an unusable header and `PDN_APRS_ERR_REFUSED` for an address AX.25 cannot carry as sent.
- `pdn_aprs_phg_values()` gives a PHG's watts, height in feet, gain in dB and directivity in degrees (-1 for omnidirectional).
- `pdn_aprs_has_diagnostic()` takes `-1` for any code, so `pdn_aprs_has_diagnostic(&p, -1, PDN_APRS_SEVERITY_ERROR)` is has-errors and `..._WARNING` has-warnings.
- `PDN_APRS_COLOR_*` constants (the area colours, which were documented but missing), a `pdn_aprs_color` enum, and `pdn_aprs_capability_token()` and `pdn_aprs_capability_value()`.
- Mic-E: the decoder sets `altitude_in_comment` on a report whose altitude was sent as a `/A=`, and the encoder writes it that way, so data read from a packet writes the bytes it was read from. The builder sets it for an altitude in feet. It is outside the vectors' neutral form.
- The diff dump tool's API view gains `has_errors`, `has_warnings`, `tnc2`, `ax25` and `phg`.

## 0.3.0 (2026-11-10)

API changes:

- New builder calls, so nothing existing changes: `pdn_aprs_build_telemetry_channels()` (a presence flag per analog channel, so a channel can be empty; `pdn_aprs_build_telemetry()` calls it with all present), `pdn_aprs_build_status_options()` with `pdn_aprs_status_options` (locator with symbol, and beam heading and power; `pdn_aprs_build_status()` calls it), and `pdn_aprs_build_message_reply_ack()` (reply-ack, `""` for support with nothing to ack). Because the public structs change as below, this still needs a minor version bump.
- `pdn_aprs_third_party.packet` now holds `2 * PDN_APRS_MAX_INFO` bytes, as text does, so the encoder takes an inner packet longer than the decoder reads off the air; only `pdn_aprs_third_party` changes layout (`pdn_aprs_data` and `pdn_aprs_packet` keep their sizes and offsets), but rebuild anything that includes `pdn_aprs.h`. `pdn_aprs_decode_third_party()` decodes such a long inner packet, which keeps no copy of its information field, and returns `PDN_APRS_ERR_ARGUMENT` for a `len` larger than `packet`.
- The `pdn_aprs_build_mic_e()` locator is now sent in upper case, as the status builder's is.
- The diff dump tool refuses (no longer reports `unsupported`) a third-party packet whose inner data is unrecognized but not empty.
- Deliberate design limits, now written in the README: text longer than 1,024 bytes and text holding a NUL are not buildable through the builder (no allocation, C strings); the diff dump tool reports them as `unsupported` and nothing else.
- New `pdn_aprs_decode_written()`: decodes an information field as `pdn_aprs_decode_info()` does, but keeps no copy of it, so it takes one longer than `PDN_APRS_MAX_INFO`. It returns `PDN_APRS_ERR_TOO_LONG` only when a part of the field is longer than the packet's data holds.
- `pdn_aprs_query` gains `latitude_text` and `longitude_text`, a general query's footprint numbers as sent, so struct layouts change: rebuild everything that includes `pdn_aprs.h`.
- `course_degrees` in `pdn_aprs_report`, `pdn_aprs_position` and `pdn_aprs_mic_e`, and `radius_miles` in `pdn_aprs_query`, are now `double`, so a program can give a course or a radius between whole numbers and the encoder rounds it to the step of the field it goes in (a compressed course to 4 degrees, once). Decoded values are still whole numbers. Code that prints them with `%u` needs a cast or `%.0f`.
- `pdn_aprs_mic_e` gains `has_altitude_m` and `altitude_m`, an altitude in metres for the Mic-E altitude bytes, rounded to the metre. `altitude_feet` is now always written as a `/A=` (vectors ruling B2); before, the encoder put it in the Mic-E bytes whenever it was a whole number of metres. Give one or the other.
- `pdn_aprs_build_message()` refuses an empty message ID (`""`), which it took as none; pass NULL for none (ruling B8).

Encoding:

- No size limit (vectors ruling E11: the rules set none). The encoder refused an information field over 512 bytes, which text received as Latin-1 and written back as UTF-8 can be; it now writes any length that fits the buffer, straight into it, and checks what it wrote with `pdn_aprs_decode_written()`. `PDN_APRS_ERR_BUFFER` means the buffer is too small to write and check the field.
- Exact bytes (vectors rulings E1-E12): the vectors' `canonical_info` is now binding, so where the spec allows several forms the encoder writes the one the Encoding rule names.
  - A general query's footprint numbers are written as sent, with or without a leading space (`?APRS? 34.0,-117.15,0200`, `-.1715`); it wrote them normalised (E1).
  - After the data extension a position, object or item report's comment is written in the order the voice frequency with its fields, the braces, the `/A=` altitude, the free text after a space, base-91 telemetry, the `!DAO!` (`j006/058/146.520MHz/A=000889 Dayton Bound`): the frequency comes first, in the bytes radios read (APRS12c ch. 18); it came after the altitude (E3).
  - A Mic-E speed of 190-199 knots is written with `/`, the printable one of its two forms; it wrote DEL (E5).
  - Mic-E status text starting with 0x1D after Rev 0 telemetry is written straight on; the `/` is only for status text that would start with 0x1D (E6).
  - No delimiter where none is needed: none before `.../...` text after a compressed position, which has no data extension to mistake it for, and none before text after a Mic-E type code that only looks like a locator (`wO91SX/G`: a locator's first two letters are A-R). The space after a frequency is written before a comment that starts with a NUL, which was taken for a tone letter (E7).
  - Snowfall under 1 inch is `.` and two digits (0.5 as `.50`); it wrote `0.5` (E8).
  - A compressed wind direction that rounds to 360 degrees is written as north (c = 0); a 358-degree wind was refused (E9). A compressed course that rounds to north (1 degree, say) is no longer refused either.
  - A `!DAO!` on a compressed position carries the digits of the position the compressed bytes report; they were taken from the position given, which can differ by a fraction of a compressed step (E2).
- A value is range-checked once rounded to the step its field holds: a humidity of 99.7% is written `h00` (100%), a speed of 999.4 knots `999`, a luminosity of 999.7 W/m2 `l000`. They were refused.
- NMEA data whose position, fix, course, speed, altitude, time or waypoint disagrees with its sentence is refused, since the sentence is written as it is and would read back with its own; the fields were not checked. Fields left unset are not checked, so data holding only a sentence is still written.
- Exact bytes, batch 2 (vectors rulings F1-F17, B1-B8, G1-G3 and the run 2 rulings; vectors e27a79f):
  - Values between steps are rounded once to the nearest step, halves away from zero, a value within 1e-6 of a half (measured in steps) being the half, and checked after rounding (F2): a course or speed, `/A=` feet, Mic-E speed and course, latitude and longitude minutes, DAO digits, weather values, RNG miles, a footprint radius, a voice frequency, and what the cs bytes carry. Negative halves went toward +infinity (-0.5 was `t000` and `/A=000000`; now `t-01` and `/A=-00001`), and halves were decided without the tolerance (58.584999999999996 minutes from 0.9764166666666666 degrees is now `58.59`). A course of -0.3 is 0 and a course of 360.4 is 360. A value the cs bytes carry rounds to the nearest step of their formula for any whole s, and is refused when that s is outside the byte (G3): a compressed speed of -0.03 knots is written as 0, -0.3 is refused, a range of 1.95 miles is written as 2. With a `!DAO!` the position is taken in DAO steps, rounded once, and split into the main digits and the DAO digit.
  - A course of 0 is written only where it reads back as 0: `000` uncompressed, refused in Mic-E and compressed reports, after rounding (F1). A compressed course needs a speed and a speed a course (F5).
  - Snowfall that rounds to a whole number is three digits (0.995 is `001`, 9.96 is `010`), and one that rounds past 999 is refused (F3).
  - A frequency after a DF bearing or storm data follows a `/` (F9).
  - Numbers given without their text (F11): a telemetry value that is a whole number 0-999 is three digits (`005`, `000`, `091`); a general query's footprint latitude and longitude that are not negative take a leading space, 0 included (F15), and the radius is rounded to whole miles.
  - A compressed weather report's GGA altitude goes in the cs bytes alone, rounded to their step, as it has no comment for a `/A=` (F14); it was refused.
  - Comment text holding a PHG, RNG or DFS after an area is refused, since it now reads back as the data extension (F7a). A Mic-E locator followed straight by base-91 telemetry or a `!DAO!` is written (F16).
  - Text after an area symbol that only looks like an area (`81019310`, whose `1` form would give colour 19) is written straight on, as a decoder does not read it as one; it was written after a `/` (E7).
  - A luminosity that rounds to 1000 W/m2 (999.5) is `l000`; it was refused.
  - A Mic-E comment starting with `/` that a `/` delimiter would still join into an altitude (`/{}` after a `/A=`) is written after a space; it was refused.
  - The ambiguity cut takes the 1e-6 tolerance in the box's units, so 80.49 degrees (29.40 minutes) is the box 29.4x (G2).
  - With ambiguity, the Mic-E longitude digits a decoder ignores hold the centre it reports (README Mic-E rule); they held the position's own digits.
  - Where neither the canonical form nor a `/` reads back, a space is written before the comment: a Mic-E comment such as `{5}` after a `/A=` would join its digits into a Mic-E altitude, and `/` is a base-91 byte itself (README Encoding rule, E12).
  - The builder passes a compressed course on as given, so it is rounded once: 301.7 degrees is `l`, where whole degrees first gave `m`. A Mic-E builder course that rounds to 0 is the spec's unknown (B1).

Decoding:

- A PHG, RNG or DFS later in an area object's comment is found as the data extension, as after a course/speed (ruling F7a: an area, like a DF bearing or storm data, describes the object, not the station's radio).
- A later Mic-E altitude is looked for before base-91 telemetry and a `!DAO!` are lifted from the status text (only the locator is read as if they were out), so bytes either side of a `!DAO!` never join into an altitude, and a `!DAO!` that shares a byte with the altitude found is not one (`"13.7V!w*@!7e}`: the altitude is `!7e}`). With `mic-e-altitude-not-first` switched off, a report with a later altitude is rejected when the text left, once telemetry and the `!DAO!` are lifted, holds an altitude formed across a place one was taken from (an altitude the bytes already held is kept); with no later altitude, a strict decoder reads the status text as a lenient one does.
- A PHG or RNG straight after a Mic-E locator and its space is lifted before a later Mic-E altitude is looked for, so its digits are never one (G1; this already held, and is now tested).
- Base-91 telemetry and a `!DAO!` come out of Mic-E status text before the locator is read, so a locator followed straight by either has no `missing-space-after-locator` (F16).

Tests and tools:

- `pdn_aprs_diffdump` gains `--encode` (neutral data in, the bytes written and what they decode to out) and `--build` (builder recipes in, the line built out), and its decode records gain the API view, as the vectors' README describes. It reads stdin and writes stdout.
- `pdn_aprs_diffdump --build` reports text longer than the C data holds (1024 bytes) as `unsupported`, not refused; refuses a telemetry sequence over 999 (B6); and gives a Mic-E altitude in metres to the builder as metres (B2).
- The vectors move to the exact-bytes branch. The runner compares the bytes written with `canonical_info` byte for byte for `equivalent` and the new `rounded` (whose data read back is not compared); it only checked that they decoded to the same data.

## 0.2.0

Brings the library into line with the rulings from differential fuzzing of all five implementations (packet-net/aprs-vectors, "Rulings from differential fuzzing: 120 cases and the rules behind them"). The vectors move to dc3854a, which adds rounds 2 to 7 of the fuzzing rulings and settles signpost and corridor braces.

API changes. Struct layouts change, so rebuild everything that includes `pdn_aprs.h`:

- `pdn_aprs_nmea` gains `comment` and `comment_len`: the text after a sentence's checksum, as sent.
- `pdn_aprs_nmea.time` and `waypoint` are now `PDN_APRS_MAX_INFO` bytes. The fraction of a second is kept as sent whatever its length (it was cut at 6 digits), and a WPL waypoint name is no longer cut at 19 characters. `sentence` is now `PDN_APRS_MAX_INFO` bytes as well, since it is always printable ASCII.
- `pdn_aprs_telemetry.sequence` is now `PDN_APRS_MAX_INFO` bytes. A sequence of 20 or more characters used to make the report invalid.
- `query_type` in `pdn_aprs_directed_query` and `pdn_aprs_query` is now `PDN_APRS_MAX_INFO` bytes, so a query type of any length is read.
- `pdn_aprs_number.text` is left empty when the number as sent is longer than it holds, and the value is still read. A telemetry value of 24 or more characters used to make the report invalid.
- `pdn_aprs_packet` grows from about 2.6 KB to 3.4 KB, and `pdn_aprs_data` from 1.8 KB to 2.6 KB.

Decoding:

- NMEA. The text after `$` must be an NMEA 0183 sentence, or it is `invalid-nmea`: printable ASCII, an address of five upper-case letters or digits (or `P` and three or more), at least one field, and no `$` or stray `*`. This is checked before the checksum. A sentence ends at its first `*` and two hex digits, the checksum is checked, and any text after it is the new `comment` (TinyTrack's `/Home Station`). A position needs both coordinates, each with a degree digit, minutes under 60 and a value in range. A time is exactly `hhmmss` with valid hours, minutes and seconds. GGA's fix is one digit. A proprietary sentence (`$PGRMC`) is kept as text, never read as RMC.
- Mic-E. The 0x1C and 0x1D data type identifiers get an `obsolete-format` info, whatever follows them. Kenwood 0xFF padding is removed only from the status text, once the destination and the nine fixed bytes have decoded: an 0xFF among the fixed bytes is out of range, and a report that fails earlier has no padding warning.
- Weather. In a position, object or item report the wind is decided first (sent as `c` and `s` fields, or missing), before any field's width. A second `s` after the wind speed is snowfall. `L` and `l` are one field. A wind direction over 360, in a `c` field or in an extension after a compressed position, is `out-of-range-value` and dropped. An extra field's value is the whole run, and it must end in a digit.
- Positions. An ambiguous latitude whose reported centre is past the pole (`90  .  N`, centred on 90 degrees 30 minutes) is `invalid-latitude`, and a Mic-E destination like it is `invalid-mic-e-destination`; a longitude whose centre is past 180 degrees (`180  .  W` under that ambiguity) is `invalid-longitude`. A DF report's bearing over 360 is `out-of-range-value`, and the whole `/BRG/NRQ` is dropped. `!DAO!` precision added to a zero latitude or longitude in the south or west (0 degrees 0 minutes S) goes south or west too; it went north or east. The latitude alone sets the ambiguity: the longitude may hold digits or spaces in the places the latitude blanks, and a space anywhere else is `invalid-longitude`. A garbled timestamp, in an object or a `/` or `@` report, is judged on the position straight after it, not on the rest of the report. A digit `!DAO!` datum is read when A and O are spaces. A signpost or corridor is the first well-formed braces wherever they are: `{`, 1-3 characters that are not braces, and `}`, the characters printable ASCII for a signpost or digits for a corridor. Braces that do not qualify are comment text and do not stop the search, so `{5<0x01>5} {AB}` has the signpost `AB` and a line object's `{AB}{5}` a 5-mile corridor. In a compressed position the latitude and longitude, and their range, are checked before the symbol code. Base-91 telemetry's binary value is the eight channels, 0-255.
- Messages. A bulletin is `BLN` then a digit or an upper-case letter. Bulletins, NWS bulletins and telemetry metadata do not take the reply-ack form: `{MM}AA` on them is `brace-in-message-text`, and the text is kept. A stray `{` in a `PARM.`, `UNIT.` or `BITS.` list stays in the list, which is still telemetry metadata. Only a message can be a directed query or telemetry metadata: bulletin and NWS bulletin text starting with `?`, `PARM.`, `UNIT.`, `EQNS.` or `BITS.` is text, with no `invalid-query` or `invalid-telemetry-metadata` info (APRS12c ch. 15, queries are "addressed to individual stations"; ch. 13, the metadata addressee is "the callsign of the station transmitting the telemetry data"). A directed query's target is one callsign (letters, digits or `-`); one space may separate it from the type, and spaces after it are padding.
- Numbers. Telemetry values, `EQNS.` coefficients and a general query's footprint take no `+`, and a footprint value's leading space is only for a positive value (APRS12c ch. 15), so a space before a minus sign is `invalid-general-query`. A coefficient of `0eN` is 0, and one that is not a finite number (`1e400`) makes the `EQNS.` a plain message. By the same rule a telemetry value too large for a double is `invalid-telemetry`, and an NMEA speed, course or altitude that large is left out.
- Capabilities. Spaces around `=` are padding. An empty token, or a control character in a value, makes the report free text.
- Status. Spaces before a `^HP` beam heading are part of the text.
- Third-party. The source inside may be any 1-9 printable ASCII characters other than `>` and `:`, and a q-construct is read only in the outer header. A defect its header may tolerate is a warning on the inner packet, and a strict decode rejects the packet as `invalid-third-party` (it gave an empty inner packet).

Encoding:

- Writes an NMEA comment after the checksum, and refuses a comment on a sentence without one.
- Writes a GGA compressed altitude under 1 foot as `!!` in the cs bytes and the altitude in `/A=` (it refused), a directed query's target after one space for a type the spec does not define and an APRSH target padded to 9 characters, a digit `!DAO!` datum, snowfall in a positionless report, a snowfall under 1 inch in hundredths (0.32 as `.32`; it refused), status text exactly as it is before `^HP`, a negative general query latitude with no space before it (it wrote one), and numbers of 1.8e19 and over (it refused them).
- Writes Mic-E status text that starts with 0x1D after a `/` (APRS12c ch. 10: status text must not start with 0x1D); it wrote it straight after the fixed bytes whenever that happened to read back.
- Refuses a DF bearing over 360, a capability value that holds a control character or starts or ends with a space, a reply-ack on a bulletin, a signpost that is not printable ASCII, a base-91 binary value over 255, and a third-party packet whose inner header has a tolerated defect.

Tests and tools:

- The neutral form reads and writes `comment` on `nmea`.
- The JSON writer shared by the tests and `pdn_aprs_diffdump` always writes valid JSON: a non-finite number becomes `null` (the dump wrote `-nan` for an `EQNS.` coefficient), a locale's decimal comma never appears, and bytes that are not UTF-8 are written as their Latin-1 code points. A new test, `json`, checks this.

## 0.1.1

- Mic-E Rev 0 binary telemetry now gets an `obsolete-format` info, as the other obsolete formats do, and a value of 255 is refused on encoding, since it would be removed as 0xFF padding on the way back in. The ruling is shared by all five implementations (packet-net/aprs-vectors, "Mic-E Rev 0 binary telemetry").

## 0.1.0

The first release.

- Decodes TNC2 / APRS-IS lines and AX.25 UI frames: header, information field, data and diagnostics, for every APRS data type the conformance vectors cover.
- Lenient decoding by default, strict decoding, and each tolerable defect switchable on its own.
- Encodes every type, refusing what the spec does not allow, and checks that what it writes reads back the same. Mic-E encoding computes the destination address.
- Builder calls for the packets a station sends: position, object, item, Mic-E, weather, message, ack and reject, bulletin, status, telemetry and its PARM, UNIT, EQNS and BITS metadata, as TNC2 lines or AX.25 frames.
- Named symbols, with an overlay helper and descriptions.
- Device identification from the aprs-deviceid database, in an optional file (`pdn_aprs_deviceid.c`, CC BY-SA 2.0 data).
- Shipped as `pdn_aprs.h` and `pdn_aprs.c`, C99, no allocation, no global state, nothing locale-dependent.
- Passes all 5,524 checks of packet-net/aprs-vectors at 16f12df, and agrees with the Rust implementation on every packet of a 6,879,893-packet APRS-IS capture.
