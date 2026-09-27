# Changelog

## Unreleased

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
