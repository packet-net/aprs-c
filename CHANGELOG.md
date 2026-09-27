# Changelog

## Unreleased

Brings the library into line with the rulings from differential fuzzing of all five implementations (packet-net/aprs-vectors, "Rulings from differential fuzzing: 120 cases and the rules behind them"). The vectors move to b41427a, which adds a second round of fuzzing rulings.

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
- Positions. An ambiguous latitude whose reported centre is past the pole (`90  .  N`, centred on 90 degrees 30 minutes) is `invalid-latitude`, and a Mic-E destination like it is `invalid-mic-e-destination`; a longitude whose centre is past 180 degrees (`180  .  W` under that ambiguity) is `invalid-longitude`. A DF report's bearing over 360 is `out-of-range-value`, and the whole `/BRG/NRQ` is dropped. `!DAO!` precision added to a zero latitude or longitude in the south or west (0 degrees 0 minutes S) goes south or west too; it went north or east. The latitude alone sets the ambiguity: the longitude may hold digits or spaces in the places the latitude blanks, and a space anywhere else is `invalid-longitude`. A garbled timestamp, in an object or a `/` or `@` report, is judged on the position straight after it, not on the rest of the report. A digit `!DAO!` datum is read when A and O are spaces. A signpost is printable ASCII. Base-91 telemetry's binary value is the eight channels, 0-255.
- Messages. A bulletin is `BLN` then a digit or an upper-case letter. Bulletins, NWS bulletins and telemetry metadata do not take the reply-ack form: `{MM}AA` on them is `brace-in-message-text`, and the text is kept. A stray `{` in a `PARM.`, `UNIT.` or `BITS.` list stays in the list, which is still telemetry metadata. A directed query's target is one callsign (letters, digits or `-`); one space may separate it from the type, and spaces after it are padding.
- Numbers. Telemetry values, `EQNS.` coefficients and a general query's footprint take no `+`. A coefficient of `0eN` is 0, and one that is not a finite number (`1e400`) makes the `EQNS.` a plain message. By the same rule a telemetry value too large for a double is `invalid-telemetry`, and an NMEA speed, course or altitude that large is left out.
- Capabilities. Spaces around `=` are padding. An empty token, or a control character in a value, makes the report free text.
- Status. Spaces before a `^HP` beam heading are part of the text.
- Third-party. The source inside may be any 1-9 printable ASCII characters other than `>` and `:`. A defect its header may tolerate is a warning on the inner packet, and a strict decode rejects the packet as `invalid-third-party` (it gave an empty inner packet).

Encoding:

- Writes an NMEA comment after the checksum, and refuses a comment on a sentence without one.
- Writes a GGA compressed altitude under 1 foot as `!!` in the cs bytes and the altitude in `/A=` (it refused), a directed query's target after one space for a type the spec does not define and an APRSH target padded to 9 characters, a digit `!DAO!` datum, snowfall in a positionless report, status text exactly as it is before `^HP`, and numbers of 1.8e19 and over (it refused them).
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
