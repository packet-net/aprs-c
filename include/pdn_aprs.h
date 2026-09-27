/*
 * pdn_aprs.h - APRS encoder and decoder in C99.
 *
 * https://github.com/packet-net/aprs-c
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tom Fanning and Packet.NET contributors
 *
 * Decodes TNC2 / APRS-IS lines and AX.25 UI frames into a packet (header, raw
 * information field, decoded data, diagnostics) and encodes data back into an
 * information field, a TNC2 line or an AX.25 frame. Checked against the
 * language-neutral conformance vectors in packet-net/aprs-vectors.
 *
 * Ground rules, for embedding in a daemon:
 *  - No dynamic allocation. The caller owns every struct and every buffer.
 *  - No global mutable state: every function is reentrant and thread-safe.
 *  - Inputs are a pointer and a length and are never read past the length;
 *    outputs go into caller buffers of stated capacity and never past it.
 *  - Strings in decoded structs are bounded, NUL-terminated fixed arrays
 *    holding UTF-8 (text that was not valid UTF-8 is read as Latin-1 and
 *    converted). A decoded packet is self-contained: it keeps no pointer into
 *    the input, so it may be copied and the input buffer reused.
 *  - Nothing is locale-dependent.
 *  - Public names start pdn_aprs_ / PDN_APRS_.
 */
#ifndef PDN_APRS_H
#define PDN_APRS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PDN_APRS_VERSION_MAJOR 0
#define PDN_APRS_VERSION_MINOR 2
#define PDN_APRS_VERSION_PATCH 0
#define PDN_APRS_VERSION "0.2.0"

/* The version of the library that was compiled, as "MAJOR.MINOR.PATCH". */
const char *pdn_aprs_version(void);

/* ------------------------------------------------------------------------ */
/* Capacities                                                               */
/* ------------------------------------------------------------------------ */

/*
 * The longest information field the decoder accepts, in bytes. APRS-IS lines
 * are at most 512 bytes and an AX.25 information field at most 256, so the
 * default holds anything valid. A longer field is refused with
 * PDN_APRS_ERR_TOO_LONG. It may be overridden with -D, but then identically
 * for every file that includes this header, since it sets struct sizes.
 */
#ifndef PDN_APRS_MAX_INFO
#define PDN_APRS_MAX_INFO 512
#endif
#if PDN_APRS_MAX_INFO < 256 || PDN_APRS_MAX_INFO > 16384
#error "PDN_APRS_MAX_INFO must be between 256 and 16384"
#endif

/* Room for any text taken from an information field, as UTF-8 plus a NUL
   (Latin-1 text can double in size when converted). */
#define PDN_APRS_TEXT_SIZE (2 * PDN_APRS_MAX_INFO + 1)
/* Room for a short fixed-width field (a name, an addressee, a message ID):
   nine bytes, possibly doubled by Latin-1 conversion, plus a NUL. */
#define PDN_APRS_NAME_SIZE 20
/* Room for an address: up to 9 characters and a NUL. */
#define PDN_APRS_ADDR_SIZE 10
/* The most path entries a header can hold. AX.25 allows 8; APRS-IS adds
   q-constructs. A TNC2 header with more is an invalid-header error. */
#ifndef PDN_APRS_MAX_PATH
#define PDN_APRS_MAX_PATH 16
#endif
/* The most diagnostics kept for one packet; any beyond are dropped. */
#define PDN_APRS_MAX_DIAGNOSTICS 24
/* Base-91 comment telemetry and telemetry reports carry up to 5 analog
   channels; telemetry metadata names up to 13 channels and gives up to 15
   equation coefficients. */
#define PDN_APRS_MAX_ANALOG 5
#define PDN_APRS_MAX_META_ITEMS 15
/* Station capabilities kept for one report; more is invalid-capabilities. */
#define PDN_APRS_MAX_CAPABILITIES 64
/* Unrecognised weather fields ("extra") kept for one report. */
#define PDN_APRS_MAX_WEATHER_EXTRA 8

/* ------------------------------------------------------------------------ */
/* Results                                                                  */
/* ------------------------------------------------------------------------ */

/* Functions that write bytes return the number written (>= 0), others
   return PDN_APRS_OK; either returns one of these on failure. */
#define PDN_APRS_OK 0
#define PDN_APRS_ERR_ARGUMENT (-1) /* a NULL pointer or an argument out of range */
#define PDN_APRS_ERR_HEADER (-2)   /* the header is unusable; the packet's diagnostics say why */
#define PDN_APRS_ERR_TOO_LONG (-3) /* the information field is longer than PDN_APRS_MAX_INFO */
#define PDN_APRS_ERR_BUFFER (-4)   /* the output buffer is too small */
#define PDN_APRS_ERR_REFUSED (-5)  /* the encoder refuses: APRS does not allow writing this data */

/* A short plain-ASCII description of a result code. */
const char *pdn_aprs_strerror(int result);

/* ------------------------------------------------------------------------ */
/* Diagnostics                                                              */
/* ------------------------------------------------------------------------ */

typedef enum pdn_aprs_severity {
    PDN_APRS_SEVERITY_INFO = 0,    /* worth knowing; nothing is wrong */
    PDN_APRS_SEVERITY_WARNING = 1, /* a defect the decoder tolerated */
    PDN_APRS_SEVERITY_ERROR = 2    /* a defect that stopped decoding */
} pdn_aprs_severity;

/* The diagnostic codes of the conformance vectors (codes.json), in order.
   pdn_aprs_code_name() gives the vectors' kebab-case id. */
typedef enum pdn_aprs_code {
    PDN_APRS_CODE_NONE = 0,
    PDN_APRS_CODE_INVALID_HEADER,
    PDN_APRS_CODE_INVALID_ADDRESS,
    PDN_APRS_CODE_EMPTY_DESTINATION,
    PDN_APRS_CODE_EMPTY_PATH_ENTRY,
    PDN_APRS_CODE_MULTIPLE_USED_MARKERS,
    PDN_APRS_CODE_NOT_APRS_FRAME,
    PDN_APRS_CODE_NUL_PADDED_ADDRESS,
    PDN_APRS_CODE_INVALID_AX25_ADDRESS_CHARACTERS,
    PDN_APRS_CODE_TOO_MANY_DIGIPEATERS,
    PDN_APRS_CODE_TRAILING_LINE_BREAK,
    PDN_APRS_CODE_NON_UTF8_TEXT,
    PDN_APRS_CODE_TRUNCATED,
    PDN_APRS_CODE_NOT_APRS,
    PDN_APRS_CODE_RESERVED_DATA_TYPE,
    PDN_APRS_CODE_OBSOLETE_FORMAT,
    PDN_APRS_CODE_OUT_OF_RANGE_VALUE,
    PDN_APRS_CODE_INVALID_TIMESTAMP,
    PDN_APRS_CODE_INVALID_POSITION,
    PDN_APRS_CODE_INVALID_LATITUDE,
    PDN_APRS_CODE_INVALID_LONGITUDE,
    PDN_APRS_CODE_LOWERCASE_HEMISPHERE,
    PDN_APRS_CODE_INVALID_SYMBOL_TABLE,
    PDN_APRS_CODE_INVALID_SYMBOL_CODE,
    PDN_APRS_CODE_INVALID_COMPRESSED_POSITION,
    PDN_APRS_CODE_DAO_WITH_AMBIGUITY,
    PDN_APRS_CODE_DATA_EXTENSION_IN_COMMENT,
    PDN_APRS_CODE_INVALID_OBJECT_NAME,
    PDN_APRS_CODE_OBJECT_NAME_NOT_PADDED,
    PDN_APRS_CODE_OBJECT_WITHOUT_TIMESTAMP,
    PDN_APRS_CODE_INVALID_ITEM_NAME,
    PDN_APRS_CODE_INCOMPLETE_WEATHER,
    PDN_APRS_CODE_WEATHER_COMMENT,
    PDN_APRS_CODE_INVALID_WEATHER,
    PDN_APRS_CODE_INVALID_MIC_E_DESTINATION,
    PDN_APRS_CODE_INVALID_MIC_E_INFORMATION,
    PDN_APRS_CODE_KENWOOD_FF_PADDING,
    PDN_APRS_CODE_MIC_E_MISSING_DEVICE_TYPE,
    PDN_APRS_CODE_INVALID_MESSAGE,
    PDN_APRS_CODE_UNPADDED_ADDRESSEE,
    PDN_APRS_CODE_MESSAGE_ID_ON_ACK,
    PDN_APRS_CODE_INVALID_TELEMETRY_METADATA,
    PDN_APRS_CODE_INVALID_QUERY,
    PDN_APRS_CODE_INVALID_TELEMETRY,
    PDN_APRS_CODE_INVALID_STATUS,
    PDN_APRS_CODE_INVALID_LOCATOR,
    PDN_APRS_CODE_INVALID_NMEA,
    PDN_APRS_CODE_NMEA_CHECKSUM_MISMATCH,
    PDN_APRS_CODE_INVALID_THIRD_PARTY,
    PDN_APRS_CODE_INVALID_GENERAL_QUERY,
    PDN_APRS_CODE_INVALID_CAPABILITIES,
    PDN_APRS_CODE_INVALID_USER_DEFINED,
    PDN_APRS_CODE_INVALID_AGRELO_DF,
    PDN_APRS_CODE_MISSING_SPACE_AFTER_LOCATOR,
    PDN_APRS_CODE_COMPRESSION_TYPE_RESERVED_BITS,
    PDN_APRS_CODE_MALFORMED_TIMESTAMP,
    PDN_APRS_CODE_POSITION_NOT_AT_START,
    PDN_APRS_CODE_NON_STANDARD_WEATHER_FIELD_WIDTH,
    PDN_APRS_CODE_WIND_FIELDS_INSTEAD_OF_EXTENSION,
    PDN_APRS_CODE_WIND_EXTENSION_AFTER_COMPRESSED,
    PDN_APRS_CODE_MIC_E_ALTITUDE_NOT_FIRST,
    PDN_APRS_CODE_BRACE_IN_MESSAGE_TEXT,
    PDN_APRS_CODE_INVALID_ADDRESSEE_CHARACTERS,
    PDN_APRS_CODE_LETTER_GROUP_BULLETIN,
    PDN_APRS_CODE_FREE_TEXT_CAPABILITIES,
    PDN_APRS_CODE_COUNT
} pdn_aprs_code;

typedef struct pdn_aprs_diagnostic {
    uint8_t severity; /* pdn_aprs_severity */
    uint8_t code;     /* pdn_aprs_code */
} pdn_aprs_diagnostic;

/* The vectors' id for a code ("trailing-line-break"), or NULL if out of range. */
const char *pdn_aprs_code_name(int code);
/* The code for a vectors' id, or PDN_APRS_CODE_NONE. */
int pdn_aprs_code_from_name(const char *name, size_t len);
/* 1 if a lenient decoder may tolerate the code with a warning. */
int pdn_aprs_code_tolerable(int code);
/* "info", "warning" or "error". */
const char *pdn_aprs_severity_name(int severity);

/* ------------------------------------------------------------------------ */
/* Symbols                                                                  */
/* ------------------------------------------------------------------------ */

/* A display symbol: the table identifier ('/' primary, '\\' alternate, or an
   overlay character 0-9 / A-Z on the alternate table) and the symbol code. */
typedef struct pdn_aprs_symbol {
    char table;
    char code;
} pdn_aprs_symbol;

/* The description from the APRS symbol tables ("Car"), or NULL if the
   symbol is not one the tables define. An overlay reads as the alternate
   table. */
const char *pdn_aprs_symbol_description(pdn_aprs_symbol symbol);
/* The symbol's name in the named-symbol list ("CAR" for PDN_APRS_SYMBOL_CAR),
   or NULL. */
const char *pdn_aprs_symbol_name(pdn_aprs_symbol symbol);
/* Places an overlay character (0-9 or A-Z) on an alternate-table symbol,
   returning 1 and setting *out; returns 0 if the overlay is not 0-9 or A-Z
   or the symbol is not on the alternate table (or already overlaid). */
int pdn_aprs_symbol_overlay(pdn_aprs_symbol symbol, char overlay, pdn_aprs_symbol *out);
/* Builds a symbol from its table and code characters. */
pdn_aprs_symbol pdn_aprs_symbol_make(char table, char code);

/* Every symbol the tables define, by name, as an initializer:
   pdn_aprs_symbol car = PDN_APRS_SYMBOL_CAR; or .symbol = PDN_APRS_SYMBOL_CAR */
#define PDN_APRS_SYMBOL_POLICE_SHERIFF {'/', '!'}
#define PDN_APRS_SYMBOL_DIGIPEATER {'/', '#'}
#define PDN_APRS_SYMBOL_PHONE {'/', '$'}
#define PDN_APRS_SYMBOL_DX_CLUSTER {'/', '%'}
#define PDN_APRS_SYMBOL_HF_GATEWAY {'/', '&'}
#define PDN_APRS_SYMBOL_SMALL_AIRCRAFT {'/', '\''}
#define PDN_APRS_SYMBOL_MOBILE_SATELLITE_GROUND_STATION {'/', '('}
#define PDN_APRS_SYMBOL_WHEELCHAIR {'/', ')'}
#define PDN_APRS_SYMBOL_SNOWMOBILE {'/', '*'}
#define PDN_APRS_SYMBOL_RED_CROSS {'/', '+'}
#define PDN_APRS_SYMBOL_BOY_SCOUTS {'/', ','}
#define PDN_APRS_SYMBOL_HOUSE {'/', '-'}
#define PDN_APRS_SYMBOL_X_MARK {'/', '.'}
#define PDN_APRS_SYMBOL_RED_DOT {'/', '/'}
#define PDN_APRS_SYMBOL_CIRCLE_0 {'/', '0'}
#define PDN_APRS_SYMBOL_CIRCLE_1 {'/', '1'}
#define PDN_APRS_SYMBOL_CIRCLE_2 {'/', '2'}
#define PDN_APRS_SYMBOL_CIRCLE_3 {'/', '3'}
#define PDN_APRS_SYMBOL_CIRCLE_4 {'/', '4'}
#define PDN_APRS_SYMBOL_CIRCLE_5 {'/', '5'}
#define PDN_APRS_SYMBOL_CIRCLE_6 {'/', '6'}
#define PDN_APRS_SYMBOL_CIRCLE_7 {'/', '7'}
#define PDN_APRS_SYMBOL_CIRCLE_8 {'/', '8'}
#define PDN_APRS_SYMBOL_CIRCLE_9 {'/', '9'}
#define PDN_APRS_SYMBOL_FIRE {'/', ':'}
#define PDN_APRS_SYMBOL_CAMPGROUND {'/', ';'}
#define PDN_APRS_SYMBOL_MOTORCYCLE {'/', '<'}
#define PDN_APRS_SYMBOL_RAILROAD_ENGINE {'/', '='}
#define PDN_APRS_SYMBOL_CAR {'/', '>'}
#define PDN_APRS_SYMBOL_FILE_SERVER {'/', '?'}
#define PDN_APRS_SYMBOL_HURRICANE_FUTURE_PREDICTION {'/', '@'}
#define PDN_APRS_SYMBOL_AID_STATION {'/', 'A'}
#define PDN_APRS_SYMBOL_BBS {'/', 'B'}
#define PDN_APRS_SYMBOL_CANOE {'/', 'C'}
#define PDN_APRS_SYMBOL_EYEBALL {'/', 'E'}
#define PDN_APRS_SYMBOL_FARM_VEHICLE {'/', 'F'}
#define PDN_APRS_SYMBOL_GRID_SQUARE {'/', 'G'}
#define PDN_APRS_SYMBOL_HOTEL {'/', 'H'}
#define PDN_APRS_SYMBOL_TCP_IP_NETWORK_STATION {'/', 'I'}
#define PDN_APRS_SYMBOL_SCHOOL {'/', 'K'}
#define PDN_APRS_SYMBOL_PC_USER {'/', 'L'}
#define PDN_APRS_SYMBOL_MAC_APRS {'/', 'M'}
#define PDN_APRS_SYMBOL_NTS_STATION {'/', 'N'}
#define PDN_APRS_SYMBOL_BALLOON {'/', 'O'}
#define PDN_APRS_SYMBOL_POLICE {'/', 'P'}
#define PDN_APRS_SYMBOL_RECREATIONAL_VEHICLE {'/', 'R'}
#define PDN_APRS_SYMBOL_SPACE_SHUTTLE {'/', 'S'}
#define PDN_APRS_SYMBOL_SSTV {'/', 'T'}
#define PDN_APRS_SYMBOL_BUS {'/', 'U'}
#define PDN_APRS_SYMBOL_AMATEUR_TV {'/', 'V'}
#define PDN_APRS_SYMBOL_NATIONAL_WEATHER_SERVICE_SITE {'/', 'W'}
#define PDN_APRS_SYMBOL_HELICOPTER {'/', 'X'}
#define PDN_APRS_SYMBOL_YACHT {'/', 'Y'}
#define PDN_APRS_SYMBOL_WIN_APRS {'/', 'Z'}
#define PDN_APRS_SYMBOL_JOGGER {'/', '['}
#define PDN_APRS_SYMBOL_DIRECTION_FINDING {'/', '\\'}
#define PDN_APRS_SYMBOL_POST_OFFICE {'/', ']'}
#define PDN_APRS_SYMBOL_LARGE_AIRCRAFT {'/', '^'}
#define PDN_APRS_SYMBOL_WEATHER_STATION {'/', '_'}
#define PDN_APRS_SYMBOL_DISH_ANTENNA {'/', '`'}
#define PDN_APRS_SYMBOL_AMBULANCE {'/', 'a'}
#define PDN_APRS_SYMBOL_BICYCLE {'/', 'b'}
#define PDN_APRS_SYMBOL_INCIDENT_COMMAND_POST {'/', 'c'}
#define PDN_APRS_SYMBOL_FIRE_DEPARTMENT {'/', 'd'}
#define PDN_APRS_SYMBOL_HORSE {'/', 'e'}
#define PDN_APRS_SYMBOL_FIRE_TRUCK {'/', 'f'}
#define PDN_APRS_SYMBOL_GLIDER {'/', 'g'}
#define PDN_APRS_SYMBOL_HOSPITAL {'/', 'h'}
#define PDN_APRS_SYMBOL_IOTA {'/', 'i'}
#define PDN_APRS_SYMBOL_JEEP {'/', 'j'}
#define PDN_APRS_SYMBOL_TRUCK {'/', 'k'}
#define PDN_APRS_SYMBOL_LAPTOP {'/', 'l'}
#define PDN_APRS_SYMBOL_MIC_E_REPEATER {'/', 'm'}
#define PDN_APRS_SYMBOL_NODE {'/', 'n'}
#define PDN_APRS_SYMBOL_EMERGENCY_OPERATIONS_CENTER {'/', 'o'}
#define PDN_APRS_SYMBOL_ROVER {'/', 'p'}
#define PDN_APRS_SYMBOL_GRID_SQUARE_ABOVE_128M {'/', 'q'}
#define PDN_APRS_SYMBOL_REPEATER {'/', 'r'}
#define PDN_APRS_SYMBOL_SHIP {'/', 's'}
#define PDN_APRS_SYMBOL_TRUCK_STOP {'/', 't'}
#define PDN_APRS_SYMBOL_EIGHTEEN_WHEELER {'/', 'u'}
#define PDN_APRS_SYMBOL_VAN {'/', 'v'}
#define PDN_APRS_SYMBOL_WATER_STATION {'/', 'w'}
#define PDN_APRS_SYMBOL_X_APRS {'/', 'x'}
#define PDN_APRS_SYMBOL_YAGI_AT_QTH {'/', 'y'}
#define PDN_APRS_SYMBOL_EMERGENCY {'\\', '!'}
#define PDN_APRS_SYMBOL_OVERLAY_DIGIPEATER {'\\', '#'}
#define PDN_APRS_SYMBOL_BANK {'\\', '$'}
#define PDN_APRS_SYMBOL_POWER_PLANT {'\\', '%'}
#define PDN_APRS_SYMBOL_GATEWAY {'\\', '&'}
#define PDN_APRS_SYMBOL_CRASH {'\\', '\''}
#define PDN_APRS_SYMBOL_CLOUDY {'\\', '('}
#define PDN_APRS_SYMBOL_FIRENET {'\\', ')'}
#define PDN_APRS_SYMBOL_SNOW {'\\', '*'}
#define PDN_APRS_SYMBOL_CHURCH {'\\', '+'}
#define PDN_APRS_SYMBOL_GIRL_SCOUTS {'\\', ','}
#define PDN_APRS_SYMBOL_OVERLAY_HOUSE {'\\', '-'}
#define PDN_APRS_SYMBOL_AMBIGUOUS {'\\', '.'}
#define PDN_APRS_SYMBOL_WAYPOINT {'\\', '/'}
#define PDN_APRS_SYMBOL_OVERLAY_CIRCLE {'\\', '0'}
#define PDN_APRS_SYMBOL_NETWORK_NODE {'\\', '8'}
#define PDN_APRS_SYMBOL_GAS_STATION {'\\', '9'}
#define PDN_APRS_SYMBOL_HAIL {'\\', ':'}
#define PDN_APRS_SYMBOL_PARK {'\\', ';'}
#define PDN_APRS_SYMBOL_ADVISORY {'\\', '<'}
#define PDN_APRS_SYMBOL_APRSTT {'\\', '='}
#define PDN_APRS_SYMBOL_OVERLAY_VEHICLE {'\\', '>'}
#define PDN_APRS_SYMBOL_INFORMATION_KIOSK {'\\', '?'}
#define PDN_APRS_SYMBOL_HURRICANE_TROPICAL_STORM {'\\', '@'}
#define PDN_APRS_SYMBOL_OVERLAY_BOX {'\\', 'A'}
#define PDN_APRS_SYMBOL_BLOWING_SNOW {'\\', 'B'}
#define PDN_APRS_SYMBOL_COAST_GUARD {'\\', 'C'}
#define PDN_APRS_SYMBOL_DRIZZLE {'\\', 'D'}
#define PDN_APRS_SYMBOL_SMOKE {'\\', 'E'}
#define PDN_APRS_SYMBOL_FREEZING_RAIN {'\\', 'F'}
#define PDN_APRS_SYMBOL_SNOW_SHOWER {'\\', 'G'}
#define PDN_APRS_SYMBOL_HAZE {'\\', 'H'}
#define PDN_APRS_SYMBOL_RAIN_SHOWER {'\\', 'I'}
#define PDN_APRS_SYMBOL_LIGHTNING {'\\', 'J'}
#define PDN_APRS_SYMBOL_KENWOOD_HT {'\\', 'K'}
#define PDN_APRS_SYMBOL_LIGHTHOUSE {'\\', 'L'}
#define PDN_APRS_SYMBOL_MARS {'\\', 'M'}
#define PDN_APRS_SYMBOL_NAVIGATION_BUOY {'\\', 'N'}
#define PDN_APRS_SYMBOL_ROCKET {'\\', 'O'}
#define PDN_APRS_SYMBOL_PARKING {'\\', 'P'}
#define PDN_APRS_SYMBOL_EARTHQUAKE {'\\', 'Q'}
#define PDN_APRS_SYMBOL_RESTAURANT {'\\', 'R'}
#define PDN_APRS_SYMBOL_SATELLITE {'\\', 'S'}
#define PDN_APRS_SYMBOL_THUNDERSTORM {'\\', 'T'}
#define PDN_APRS_SYMBOL_SUNNY {'\\', 'U'}
#define PDN_APRS_SYMBOL_VORTAC {'\\', 'V'}
#define PDN_APRS_SYMBOL_OVERLAY_NWS_SITE {'\\', 'W'}
#define PDN_APRS_SYMBOL_PHARMACY {'\\', 'X'}
#define PDN_APRS_SYMBOL_OVERLAY_RADIO {'\\', 'Y'}
#define PDN_APRS_SYMBOL_WALL_CLOUD {'\\', '['}
#define PDN_APRS_SYMBOL_AIRCRAFT_WITH_HEADING {'\\', '^'}
#define PDN_APRS_SYMBOL_WEATHER_STATION_WITH_DIGIPEATER {'\\', '_'}
#define PDN_APRS_SYMBOL_RAIN {'\\', '`'}
#define PDN_APRS_SYMBOL_OVERLAY_DIAMOND {'\\', 'a'}
#define PDN_APRS_SYMBOL_BLOWING_DUST {'\\', 'b'}
#define PDN_APRS_SYMBOL_OVERLAY_CIVIL_DEFENSE {'\\', 'c'}
#define PDN_APRS_SYMBOL_DX_SPOT {'\\', 'd'}
#define PDN_APRS_SYMBOL_SLEET {'\\', 'e'}
#define PDN_APRS_SYMBOL_FUNNEL_CLOUD {'\\', 'f'}
#define PDN_APRS_SYMBOL_GALE_FLAGS {'\\', 'g'}
#define PDN_APRS_SYMBOL_STORE {'\\', 'h'}
#define PDN_APRS_SYMBOL_POINT_OF_INTEREST {'\\', 'i'}
#define PDN_APRS_SYMBOL_WORK_ZONE {'\\', 'j'}
#define PDN_APRS_SYMBOL_SPECIAL_VEHICLE {'\\', 'k'}
#define PDN_APRS_SYMBOL_AREA {'\\', 'l'}
#define PDN_APRS_SYMBOL_VALUE_SIGN {'\\', 'm'}
#define PDN_APRS_SYMBOL_TRIANGLE {'\\', 'n'}
#define PDN_APRS_SYMBOL_SMALL_CIRCLE {'\\', 'o'}
#define PDN_APRS_SYMBOL_PARTLY_CLOUDY {'\\', 'p'}
#define PDN_APRS_SYMBOL_RESTROOMS {'\\', 'r'}
#define PDN_APRS_SYMBOL_OVERLAY_SHIP {'\\', 's'}
#define PDN_APRS_SYMBOL_TORNADO {'\\', 't'}
#define PDN_APRS_SYMBOL_OVERLAY_TRUCK {'\\', 'u'}
#define PDN_APRS_SYMBOL_OVERLAY_VAN {'\\', 'v'}
#define PDN_APRS_SYMBOL_FLOODING {'\\', 'w'}
#define PDN_APRS_SYMBOL_WRECK {'\\', 'x'}
#define PDN_APRS_SYMBOL_SKYWARN {'\\', 'y'}
#define PDN_APRS_SYMBOL_SHELTER {'\\', 'z'}
#define PDN_APRS_SYMBOL_FOG {'\\', '{'}

/* ------------------------------------------------------------------------ */
/* Header                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct pdn_aprs_path_entry {
    char call[PDN_APRS_ADDR_SIZE]; /* "WIDE2-1", without any * marker */
    uint8_t used;                  /* marked used (*, or the AX.25 H bit), or before one that is */
    uint8_t marked;                /* this entry itself carried the marker */
} pdn_aprs_path_entry;

typedef struct pdn_aprs_header {
    char source[PDN_APRS_ADDR_SIZE];
    char destination[PDN_APRS_ADDR_SIZE];
    uint8_t path_count;
    pdn_aprs_path_entry path[PDN_APRS_MAX_PATH];
    /* Index into path of an APRS-IS q-construct (qAR, qAr, qAC, ...), or -1.
       The entry after it, if any, is the q-construct's station. */
    int8_t q_construct;
} pdn_aprs_header;

/* ------------------------------------------------------------------------ */
/* Decoded data                                                             */
/* ------------------------------------------------------------------------ */

typedef enum pdn_aprs_type {
    PDN_APRS_TYPE_UNRECOGNIZED = 0,
    PDN_APRS_TYPE_POSITION,
    PDN_APRS_TYPE_MIC_E,
    PDN_APRS_TYPE_OBJECT,
    PDN_APRS_TYPE_ITEM,
    PDN_APRS_TYPE_MESSAGE,
    PDN_APRS_TYPE_ACK,
    PDN_APRS_TYPE_REJECT,
    PDN_APRS_TYPE_BULLETIN,
    PDN_APRS_TYPE_NWS_BULLETIN,
    PDN_APRS_TYPE_TELEMETRY_NAMES,
    PDN_APRS_TYPE_TELEMETRY_UNITS,
    PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS,
    PDN_APRS_TYPE_TELEMETRY_BITS,
    PDN_APRS_TYPE_DIRECTED_QUERY,
    PDN_APRS_TYPE_STATUS,
    PDN_APRS_TYPE_TELEMETRY,
    PDN_APRS_TYPE_WEATHER,
    PDN_APRS_TYPE_RAW_WEATHER,
    PDN_APRS_TYPE_NMEA,
    PDN_APRS_TYPE_MAIDENHEAD_BEACON,
    PDN_APRS_TYPE_QUERY,
    PDN_APRS_TYPE_CAPABILITIES,
    PDN_APRS_TYPE_THIRD_PARTY,
    PDN_APRS_TYPE_USER_DEFINED,
    PDN_APRS_TYPE_TEST,
    PDN_APRS_TYPE_AGRELO_DF,
    PDN_APRS_TYPE_COUNT
} pdn_aprs_type;

/* The vectors' name for a type ("mic-e"), or NULL. */
const char *pdn_aprs_type_name(int type);

typedef enum pdn_aprs_reason {
    PDN_APRS_REASON_EMPTY = 0,          /* an empty information field */
    PDN_APRS_REASON_NOT_APRS,           /* the first byte is not a data type identifier */
    PDN_APRS_REASON_RESERVED_DATA_TYPE, /* &, + or . */
    PDN_APRS_REASON_MALFORMED           /* an error stopped decoding; see the diagnostics */
} pdn_aprs_reason;

typedef enum pdn_aprs_mic_e_message {
    PDN_APRS_MIC_E_OFF_DUTY = 0, /* M0 */
    PDN_APRS_MIC_E_EN_ROUTE,     /* M1 */
    PDN_APRS_MIC_E_IN_SERVICE,   /* M2 */
    PDN_APRS_MIC_E_RETURNING,    /* M3 */
    PDN_APRS_MIC_E_COMMITTED,    /* M4 */
    PDN_APRS_MIC_E_SPECIAL,      /* M5 */
    PDN_APRS_MIC_E_PRIORITY,     /* M6 */
    PDN_APRS_MIC_E_CUSTOM0,
    PDN_APRS_MIC_E_CUSTOM1,
    PDN_APRS_MIC_E_CUSTOM2,
    PDN_APRS_MIC_E_CUSTOM3,
    PDN_APRS_MIC_E_CUSTOM4,
    PDN_APRS_MIC_E_CUSTOM5,
    PDN_APRS_MIC_E_CUSTOM6,
    PDN_APRS_MIC_E_EMERGENCY,
    PDN_APRS_MIC_E_UNKNOWN /* standard and custom bits mixed */
} pdn_aprs_mic_e_message;

/* Compressed position type byte (APRS12c ch. 9). */
typedef struct pdn_aprs_compression {
    uint8_t fix;    /* PDN_APRS_FIX_OLD, PDN_APRS_FIX_CURRENT */
    uint8_t source; /* PDN_APRS_NMEA_OTHER, _GLL, _GGA, _RMC */
    uint8_t origin; /* PDN_APRS_ORIGIN_* (0-7) */
} pdn_aprs_compression;
#define PDN_APRS_FIX_OLD 0
#define PDN_APRS_FIX_CURRENT 1
#define PDN_APRS_NMEA_OTHER 0
#define PDN_APRS_NMEA_GLL 1
#define PDN_APRS_NMEA_GGA 2
#define PDN_APRS_NMEA_RMC 3
#define PDN_APRS_ORIGIN_COMPRESSED 0
#define PDN_APRS_ORIGIN_TNC_BEACON_TEXT 1
#define PDN_APRS_ORIGIN_SOFTWARE 2
#define PDN_APRS_ORIGIN_RESERVED3 3
#define PDN_APRS_ORIGIN_KPC3 4
#define PDN_APRS_ORIGIN_PICO 5
#define PDN_APRS_ORIGIN_OTHER_TRACKER 6
#define PDN_APRS_ORIGIN_DIGIPEATER_CONVERSION 7

/* Power, height, gain, directivity: codes as sent (APRS12c ch. 7). */
typedef struct pdn_aprs_phg {
    uint8_t power;            /* 0-9 */
    uint8_t height;           /* 0 for '0' on through the ASCII table: '~' is 78 */
    uint8_t gain;             /* 0-9 */
    uint8_t directivity;      /* 0-9 */
    uint8_t beacons_per_hour; /* PHGR: 1-9, then 10 for 'A' up to 35 for 'Z'; 0 for plain PHG */
} pdn_aprs_phg;

/* Omni-DF signal strength, height, gain, directivity: codes as sent. */
typedef struct pdn_aprs_dfs {
    uint8_t strength, height, gain, directivity;
} pdn_aprs_dfs;

/* Area object (APRS12c ch. 11). */
typedef struct pdn_aprs_area {
    uint8_t shape;      /* PDN_APRS_AREA_* 0-9 */
    uint8_t color;      /* PDN_APRS_COLOR_* 0-15 */
    uint8_t lat_offset; /* yy, as sent */
    uint8_t lon_offset; /* xx, as sent */
    uint8_t has_corridor;
    uint16_t corridor_width_miles; /* {nnn} after a line */
} pdn_aprs_area;
#define PDN_APRS_AREA_OPEN_CIRCLE 0
#define PDN_APRS_AREA_LINE_DOWN_RIGHT 1
#define PDN_APRS_AREA_OPEN_ELLIPSE 2
#define PDN_APRS_AREA_OPEN_TRIANGLE 3
#define PDN_APRS_AREA_OPEN_BOX 4
#define PDN_APRS_AREA_FILLED_CIRCLE 5
#define PDN_APRS_AREA_LINE_DOWN_LEFT 6
#define PDN_APRS_AREA_FILLED_ELLIPSE 7
#define PDN_APRS_AREA_FILLED_TRIANGLE 8
#define PDN_APRS_AREA_FILLED_BOX 9
/* Colors 0-7 are high intensity (black, blue, green, cyan, red, violet,
   yellow, gray); 8-15 the same at low intensity. */

/* DF report bearing and number/range/quality (APRS12c ch. 7). */
typedef struct pdn_aprs_df_bearing {
    uint16_t bearing_degrees;
    uint8_t number, range, quality;
} pdn_aprs_df_bearing;

/* Storm data (APRS12c ch. 12). */
typedef struct pdn_aprs_storm {
    uint8_t type; /* PDN_APRS_STORM_* */
    uint16_t sustained_wind_knots;
    uint16_t gust_knots;
    uint16_t central_pressure_mbar;
    uint16_t hurricane_radius_nm;
    uint16_t tropical_storm_radius_nm;
    uint8_t has_whole_gale_radius;
    uint16_t whole_gale_radius_nm;
} pdn_aprs_storm;
#define PDN_APRS_STORM_TROPICAL_STORM 0 /* TS */
#define PDN_APRS_STORM_HURRICANE 1      /* HC */
#define PDN_APRS_STORM_TROPICAL_DEPRESSION 2 /* TD */

/* !DAO! datum and precision. */
typedef struct pdn_aprs_dao {
    /* 'W' for WGS84, as sent (upper case); or a digit 0-9, a locally
       defined datum, which carries no added precision */
    char datum;
    uint8_t precision; /* PDN_APRS_DAO_* */
} pdn_aprs_dao;
#define PDN_APRS_DAO_NONE 0
#define PDN_APRS_DAO_THOUSANDTHS 1
#define PDN_APRS_DAO_BASE91 2

/* Base-91 comment telemetry |ss1122...| (APRS12c ch. 13). */
typedef struct pdn_aprs_comment_telemetry {
    uint16_t sequence;
    uint8_t analog_count; /* 1-5 */
    uint16_t analog[PDN_APRS_MAX_ANALOG];
    uint8_t has_digital;
    /* the eight binary channels, 0-255, B1 the least significant bit; bits
       9-13 of the value sent are reserved and ignored */
    uint16_t digital;
} pdn_aprs_comment_telemetry;

/* APRS 1.2 voice frequency (APRS12c ch. 18). */
typedef struct pdn_aprs_frequency {
    double mhz;
    uint8_t tone;        /* PDN_APRS_TONE_* */
    uint16_t tone_value; /* for TONE, CTCSS and DCS */
    uint8_t has_offset;
    int16_t offset_khz;
    uint8_t has_range;
    uint8_t range;        /* R25m: 25 */
    uint8_t range_km;     /* the range is in km (R25k) */
    uint8_t narrow;       /* lower-case tone letter: narrow modulation */
    uint8_t ten_khz_resolution; /* "146.52 MHz": the frequency to 10 kHz */
} pdn_aprs_frequency;
#define PDN_APRS_TONE_NONE 0
#define PDN_APRS_TONE_OFF 1
#define PDN_APRS_TONE_TONE 2
#define PDN_APRS_TONE_CTCSS 3
#define PDN_APRS_TONE_DCS 4
#define PDN_APRS_TONE_BURST 5 /* 1750 Hz tone burst */

/* Weather values, indexed by PDN_APRS_WX_*. Units are as APRS sends them;
   nothing is converted except a compressed position's cs-byte wind speed,
   which is knots on air and given here in mph. */
enum {
    PDN_APRS_WX_WIND_DIRECTION = 0, /* degrees */
    PDN_APRS_WX_WIND_SPEED,         /* mph */
    PDN_APRS_WX_WIND_GUST,          /* mph */
    PDN_APRS_WX_TEMPERATURE,        /* degrees F */
    PDN_APRS_WX_RAIN_1H,            /* inches */
    PDN_APRS_WX_RAIN_24H,           /* inches */
    PDN_APRS_WX_RAIN_MIDNIGHT,      /* inches */
    PDN_APRS_WX_HUMIDITY,           /* percent */
    PDN_APRS_WX_PRESSURE,           /* millibars */
    PDN_APRS_WX_LUMINOSITY,         /* watts per square metre */
    PDN_APRS_WX_SNOW_24H,           /* inches */
    PDN_APRS_WX_RAIN_RAW,           /* raw rain counter */
    PDN_APRS_WX_COUNT
};

typedef struct pdn_aprs_weather_extra {
    char letter;    /* a field letter the spec does not define */
    char value[24]; /* as sent: "-01" */
} pdn_aprs_weather_extra;

typedef struct pdn_aprs_weather {
    uint8_t has[PDN_APRS_WX_COUNT];
    double value[PDN_APRS_WX_COUNT];
    char software; /* APRS software type letter, or 0 */
    char unit[5];  /* weather unit type, 2-4 characters, or "" */
    uint8_t extra_count;
    pdn_aprs_weather_extra extra[PDN_APRS_MAX_WEATHER_EXTRA];
} pdn_aprs_weather;

/* The vectors' field name for a weather index ("temperature_f"). */
const char *pdn_aprs_weather_field_name(int index);

/*
 * A report with a position: a position report, a Mic-E report, an object or
 * an item. Each has_ flag says whether the field after it was sent.
 */
typedef struct pdn_aprs_report {
    /* Position reports and objects: the timestamp as sent ("092345z",
       "092345/", "234517h"), or "". */
    char timestamp[8];
    /* Position reports: sent with = or @ (APRS messaging capable). */
    uint8_t messaging;
    /* Objects and items. */
    char name[PDN_APRS_NAME_SIZE];
    uint8_t killed;
    /* Mic-E. */
    uint8_t mic_e_message;    /* pdn_aprs_mic_e_message */
    uint8_t old_data;         /* ' rather than ` */
    char type_code;           /* the device type code: ` ' > ] or space; 0 if none */
    char device_suffix[3];    /* "_%", "=" ...; "" if none */
    char locator[7];          /* Maidenhead locator from the status text, upper case */
    uint8_t has_legacy_telemetry;
    uint8_t legacy_telemetry[5]; /* obsolete 0x1d binary telemetry */
    uint8_t destination_ssid; /* the destination SSID (digipeater path code), 0 if none */

    /* Positioned fields. */
    double latitude;  /* degrees, north positive */
    double longitude; /* degrees, east positive */
    uint8_t ambiguity; /* 1-4 digits blanked, 0 for none */
    pdn_aprs_symbol symbol;
    uint8_t compressed;
    uint8_t has_compression;
    pdn_aprs_compression compression;
    uint8_t has_course;
    uint16_t course_degrees;
    uint8_t has_speed;
    double speed_knots;
    uint8_t has_altitude;
    double altitude_feet;
    uint8_t has_phg;
    pdn_aprs_phg phg;
    uint8_t has_range;
    double range_miles;
    uint8_t has_dfs;
    pdn_aprs_dfs dfs;
    uint8_t has_area;
    pdn_aprs_area area;
    uint8_t has_df_bearing;
    pdn_aprs_df_bearing df_bearing;
    uint8_t has_storm;
    pdn_aprs_storm storm;
    uint8_t has_dao;
    pdn_aprs_dao dao;
    uint8_t has_telemetry;
    pdn_aprs_comment_telemetry telemetry;
    uint8_t has_frequency;
    pdn_aprs_frequency frequency;
    uint8_t has_weather;
    pdn_aprs_weather weather;
    char signpost[16]; /* signpost overlay text, 1-3 characters, or "" */
    uint16_t comment_len;
    char comment[PDN_APRS_TEXT_SIZE];
} pdn_aprs_report;

/* A message, bulletin or NWS bulletin. */
typedef struct pdn_aprs_message {
    char addressee[PDN_APRS_NAME_SIZE];
    char message_id[PDN_APRS_NAME_SIZE]; /* "" if none */
    /* Reply-ack (APRS12c ch. 14): has_reply_ack with an empty reply_ack says
       the sender supports reply-acks but has nothing to ack. */
    uint8_t has_reply_ack;
    char reply_ack[PDN_APRS_NAME_SIZE];
    uint16_t text_len;
    char text[PDN_APRS_TEXT_SIZE];
} pdn_aprs_message;

/* An ack or a reject. */
typedef struct pdn_aprs_ack {
    char addressee[PDN_APRS_NAME_SIZE];
    char id[PDN_APRS_NAME_SIZE]; /* the message ID acked or rejected */
    uint8_t has_reply_ack;
    char reply_ack[PDN_APRS_NAME_SIZE];
} pdn_aprs_ack;

/* A directed query (APRS12c ch. 15). */
typedef struct pdn_aprs_directed_query {
    char addressee[PDN_APRS_NAME_SIZE];
    /* "APRSD", "PING?" ..., or any other type of upper-case letters, which
       may be as long as the text */
    char query_type[PDN_APRS_MAX_INFO];
    char target[PDN_APRS_NAME_SIZE]; /* the callsign asked about, or "" */
} pdn_aprs_directed_query;

/* A number as sent: its value and, for identical re-encoding, its text. */
typedef struct pdn_aprs_number {
    double value;    /* always a finite number */
    uint8_t is_null; /* an empty telemetry value */
    /* as sent ("073", ".53"); "" to format the value, and "" too when the
       text sent is longer than fits here (the value is still read) */
    char text[24];
} pdn_aprs_number;

/* Telemetry metadata: PARM., UNIT., EQNS. or BITS. (APRS12c ch. 13). */
typedef struct pdn_aprs_telemetry_meta {
    char addressee[PDN_APRS_NAME_SIZE];
    char message_id[PDN_APRS_NAME_SIZE]; /* "" if none */
    /* Names and units: count strings in text, the i-th at text + offset[i]
       (NUL-terminated, length[i] bytes). Coefficients: count numbers. */
    uint8_t count;
    uint16_t offset[PDN_APRS_MAX_META_ITEMS];
    uint16_t length[PDN_APRS_MAX_META_ITEMS];
    pdn_aprs_number coefficient[PDN_APRS_MAX_META_ITEMS];
    /* Bit sense: eight '0'/'1' characters, B1 first, and the project title
       (project_len bytes of text). */
    char bits[9];
    uint16_t project_len;
    char text[PDN_APRS_TEXT_SIZE + PDN_APRS_MAX_META_ITEMS]; /* names/units, or the project title */
} pdn_aprs_telemetry_meta;

/* The i-th name or unit of a PARM. or UNIT. message, or NULL. */
const char *pdn_aprs_meta_item(const pdn_aprs_telemetry_meta *meta, unsigned i, size_t *len);

/* A status report (APRS12c ch. 16). */
typedef struct pdn_aprs_status {
    char timestamp[8];      /* DHM zulu, or "" */
    char locator[7];        /* Maidenhead locator, upper case, or "" */
    pdn_aprs_symbol symbol; /* after the locator */
    uint8_t has_beam;
    char beam_heading;      /* meteor scatter ^HP: heading code */
    char beam_power;        /* power code */
    uint16_t text_len;
    char text[PDN_APRS_TEXT_SIZE]; /* as sent, spaces before a beam heading included */
} pdn_aprs_status;

/* A telemetry report T#... (APRS12c ch. 13). */
typedef struct pdn_aprs_telemetry {
    /* as sent: "005", "MIC", "51752"; letters and digits of any length */
    char sequence[PDN_APRS_MAX_INFO];
    uint8_t analog_count; /* 1-5 */
    pdn_aprs_number analog[PDN_APRS_MAX_ANALOG];
    uint8_t has_bits;
    char bits[9]; /* eight '0'/'1' characters, B1 first */
    uint16_t comment_len;
    char comment[PDN_APRS_TEXT_SIZE];
} pdn_aprs_telemetry;

/* A positionless weather report _MDHM... (APRS12c ch. 12). */
typedef struct pdn_aprs_weather_report {
    char timestamp[9]; /* MDHM, "10090556" */
    pdn_aprs_weather weather;
    uint16_t comment_len;
    char comment[PDN_APRS_TEXT_SIZE];
} pdn_aprs_weather_report;

/* Raw weather station data. */
typedef struct pdn_aprs_raw_weather {
    uint8_t format; /* PDN_APRS_RAW_WX_* */
    uint16_t data_len;
    char data[PDN_APRS_TEXT_SIZE];
} pdn_aprs_raw_weather;
#define PDN_APRS_RAW_WX_PEET_BROS_HASH 0    /* # */
#define PDN_APRS_RAW_WX_PEET_BROS_STAR 1    /* * */
#define PDN_APRS_RAW_WX_ULTIMETER_PACKET 2  /* $ULTW */
#define PDN_APRS_RAW_WX_ULTIMETER_LOGGING 3 /* !! */

/* A raw NMEA 0183 sentence (APRS12c ch. 5, 6). */
typedef struct pdn_aprs_nmea {
    uint16_t sentence_len;
    /* without the $, up to and including any *hh checksum; printable ASCII */
    char sentence[PDN_APRS_MAX_INFO];
    uint8_t has_checksum;
    uint8_t has_position;
    double latitude, longitude;
    uint8_t has_fix;
    uint8_t fix_valid;
    uint8_t has_course;
    double course_degrees;
    uint8_t has_speed;
    double speed_knots;
    uint8_t has_altitude;
    double altitude_m;
    /* "HH:MM:SS", then any fraction of a second as sent, less trailing
       zeros ("15:40:27.123456789"); or "" */
    char time[PDN_APRS_MAX_INFO];
    char waypoint[PDN_APRS_MAX_INFO]; /* WPL waypoint name, or "" */
    /* Text after the checksum (TinyTrack's "/Home Station"), as sent. */
    uint16_t comment_len;
    char comment[PDN_APRS_TEXT_SIZE];
} pdn_aprs_nmea;

/* A Maidenhead locator beacon [IO91SX] (obsolete). */
typedef struct pdn_aprs_maidenhead {
    char locator[7];
    uint16_t comment_len;
    char comment[PDN_APRS_TEXT_SIZE];
} pdn_aprs_maidenhead;

/* A general query ?APRS? (APRS12c ch. 15). */
typedef struct pdn_aprs_query {
    char query_type[PDN_APRS_MAX_INFO]; /* upper-case letters */
    uint8_t has_footprint;
    double latitude, longitude;
    uint16_t radius_miles;
} pdn_aprs_query;

/* Station capabilities: count items, each a token and an optional value,
   all stored in text. */
typedef struct pdn_aprs_capabilities {
    uint8_t count;
    struct {
        uint16_t token_offset, token_length;
        uint8_t has_value;
        uint16_t value_offset, value_length;
    } item[PDN_APRS_MAX_CAPABILITIES];
    char text[PDN_APRS_TEXT_SIZE + 2 * PDN_APRS_MAX_CAPABILITIES];
} pdn_aprs_capabilities;

/* A third-party packet }...: the encapsulated packet in TNC2 form, as
   received. pdn_aprs_decode_third_party() decodes it. Its source may be 1-9
   printable ASCII characters other than > and : (APRS12c ch. 17). A defect
   its header may tolerate (several used markers) is a warning on the inner
   packet, and a strict decode rejects the whole packet as
   invalid-third-party. A q-construct is read only in the outer header, so
   the inner packet's q_construct is always -1. */
typedef struct pdn_aprs_third_party {
    uint16_t len;
    uint8_t packet[PDN_APRS_MAX_INFO];
} pdn_aprs_third_party;

/* User-defined data {UX... (APRS12c ch. 19): bytes as sent. */
typedef struct pdn_aprs_user_defined {
    char user_id;
    char packet_type;
    uint16_t data_len;
    uint8_t data[PDN_APRS_MAX_INFO];
} pdn_aprs_user_defined;

/* Invalid or test data ,... */
typedef struct pdn_aprs_test {
    uint16_t data_len;
    char data[PDN_APRS_TEXT_SIZE];
} pdn_aprs_test;

/* Agrelo DFJr / MicroFinder %bbb/q. */
typedef struct pdn_aprs_agrelo {
    uint16_t bearing_degrees;
    uint8_t quality;
} pdn_aprs_agrelo;

typedef struct pdn_aprs_data {
    uint8_t type;   /* pdn_aprs_type */
    uint8_t reason; /* for PDN_APRS_TYPE_UNRECOGNIZED: pdn_aprs_reason */
    union {
        pdn_aprs_report report;           /* position, mic-e, object, item */
        pdn_aprs_message message;         /* message, bulletin, nws-bulletin */
        pdn_aprs_ack ack;                 /* ack, reject */
        pdn_aprs_directed_query directed_query;
        pdn_aprs_telemetry_meta meta;     /* telemetry-names/-units/-coefficients/-bits */
        pdn_aprs_status status;
        pdn_aprs_telemetry telemetry;
        pdn_aprs_weather_report weather;
        pdn_aprs_raw_weather raw_weather;
        pdn_aprs_nmea nmea;
        pdn_aprs_maidenhead maidenhead;
        pdn_aprs_query query;
        pdn_aprs_capabilities capabilities;
        pdn_aprs_third_party third_party;
        pdn_aprs_user_defined user_defined;
        pdn_aprs_test test;
        pdn_aprs_agrelo agrelo;
    } as;
} pdn_aprs_data;

/* ------------------------------------------------------------------------ */
/* Decoding                                                                 */
/* ------------------------------------------------------------------------ */

/*
 * A device identification table, in the form of the aprs-deviceid database
 * (github.com/aprsorg/aprs-deviceid). The optional pdn_aprs_deviceid.c
 * provides one generated from it; an application may also build its own.
 * Mic-E device suffixes are only recognised against a table: without one
 * they stay in the comment.
 */
typedef struct pdn_aprs_device_entry {
    /* tocalls: the destination pattern ('?' any character, 'n' a digit,
       '*' anything after); mice: the two-character suffix; mice_legacy: the
       type code, then the one-character suffix if any (">", ">=", "]="). */
    const char *key;
    const char *vendor; /* UTF-8, or NULL */
    const char *model;  /* UTF-8, or NULL */
    const char *class_; /* "ht", "rig", "tracker" ... or NULL */
} pdn_aprs_device_entry;

typedef struct pdn_aprs_device_table {
    const pdn_aprs_device_entry *tocalls;
    size_t tocall_count;
    const pdn_aprs_device_entry *mice;
    size_t mice_count;
    const pdn_aprs_device_entry *mice_legacy;
    size_t mice_legacy_count;
} pdn_aprs_device_table;

/*
 * Decode options. A zeroed struct (or NULL) decodes leniently: every
 * tolerable defect is accepted with a warning.
 */
typedef struct pdn_aprs_decode_options {
    /* Strict: reject every tolerable defect. */
    uint8_t strict;
    /* Reject individual tolerable defects: reject[PDN_APRS_CODE_X] = 1. */
    uint8_t reject[PDN_APRS_CODE_COUNT];
    /* The device table, for Mic-E device suffixes; NULL for none. The
       optional pdn_aprs_deviceid.c provides pdn_aprs_devices(). */
    const pdn_aprs_device_table *devices;
} pdn_aprs_decode_options;

typedef struct pdn_aprs_packet {
    /* 1 when the header decoded; 0 when it is unusable, in which case
       data is unrecognized and the diagnostics say why. */
    uint8_t header_ok;
    pdn_aprs_header header;
    /* The information field as received, including any trailing CR/LF. */
    uint16_t info_len;
    uint8_t info[PDN_APRS_MAX_INFO];
    pdn_aprs_data data;
    /* Every diagnostic, the header's first. */
    uint8_t diagnostic_count;
    pdn_aprs_diagnostic diagnostics[PDN_APRS_MAX_DIAGNOSTICS];
} pdn_aprs_packet;

/*
 * Decodes a TNC2 / APRS-IS line "SRC>DEST,PATH:info" (without a line end).
 * Returns PDN_APRS_OK, PDN_APRS_ERR_HEADER (the packet still says why),
 * PDN_APRS_ERR_TOO_LONG or PDN_APRS_ERR_ARGUMENT. options may be NULL.
 */
int pdn_aprs_decode_tnc2(const void *line, size_t len, const pdn_aprs_decode_options *options,
                         pdn_aprs_packet *packet);

/*
 * Decodes an AX.25 UI frame as KISS carries it: addresses, control, PID and
 * information field, without flags or FCS.
 */
int pdn_aprs_decode_ax25(const void *frame, size_t len, const pdn_aprs_decode_options *options,
                         pdn_aprs_packet *packet);

/*
 * Decodes an information field on its own, with the given header (the
 * destination matters for Mic-E). header may be NULL for N0CALL>APZ001.
 */
int pdn_aprs_decode_info(const pdn_aprs_header *header, const void *info, size_t len,
                         const pdn_aprs_decode_options *options, pdn_aprs_packet *packet);

/*
 * Decodes an information field as pdn_aprs_decode_info does, but keeps no
 * copy of it in the packet (info_len is 0), so it may be longer than
 * PDN_APRS_MAX_INFO. The encoder writes a field of any length (text received
 * as Latin-1 is written back as UTF-8, up to twice as long), and this reads
 * one back. Returns PDN_APRS_ERR_TOO_LONG, the data unrecognized, if a part
 * of the field is longer than the packet's data holds.
 */
int pdn_aprs_decode_written(const pdn_aprs_header *header, const void *info, size_t len,
                            const pdn_aprs_decode_options *options, pdn_aprs_packet *packet);

/*
 * Decodes the packet inside a third-party packet (data.type
 * PDN_APRS_TYPE_THIRD_PARTY) into inner, which has its own header, data and
 * diagnostics.
 */
int pdn_aprs_decode_third_party(const pdn_aprs_data *outer, const pdn_aprs_decode_options *options,
                                pdn_aprs_packet *inner);

/* Sets options to strict decoding. */
void pdn_aprs_options_strict(pdn_aprs_decode_options *options);

/* 1 if the packet has a diagnostic of this code (and severity, or -1 for any). */
int pdn_aprs_has_diagnostic(const pdn_aprs_packet *packet, int code, int severity);

/* ------------------------------------------------------------------------ */
/* Device identification                                                    */
/* ------------------------------------------------------------------------ */

typedef struct pdn_aprs_device {
    const char *vendor; /* NULL if unknown */
    const char *model;  /* NULL if unknown */
    const char *class_; /* "ht", "rig", "tracker" ... or NULL */
} pdn_aprs_device;

/*
 * Identifies the sending device from the aprs-deviceid database: Mic-E
 * reports by their type code and suffix, others by the destination address
 * (tocall). Returns 1 and fills device if found, else 0. With a NULL table
 * the device is always unknown.
 */
int pdn_aprs_identify_device(const pdn_aprs_device_table *table, const pdn_aprs_packet *packet,
                             pdn_aprs_device *device);

/* Provided by the optional pdn_aprs_deviceid.c (CC BY-SA 2.0 data). */
const pdn_aprs_device_table *pdn_aprs_devices(void);

/* ------------------------------------------------------------------------ */
/* Encoding                                                                 */
/* ------------------------------------------------------------------------ */

/* Encode options. A zeroed struct (or NULL) is the default. */
typedef struct pdn_aprs_encode_options {
    /* The device table the packet's readers use. The encoder checks what it
       writes by decoding it; with the table, a Mic-E comment that happens to
       end in a known device suffix is refused rather than written, since it
       would read back as that suffix. NULL checks only the report's own
       suffix. */
    const pdn_aprs_device_table *devices;
} pdn_aprs_encode_options;

typedef struct pdn_aprs_encoded {
    size_t len; /* bytes written */
    /* Mic-E: the destination address the encoder computed ("S32UVT"). */
    char destination[PDN_APRS_ADDR_SIZE];
    /* Why the encoder refused, as plain ASCII, or NULL. */
    const char *reason;
} pdn_aprs_encoded;

/*
 * Encodes data as an information field. Writes at most cap bytes and no NUL.
 * There is no limit on the length but cap: a field may be longer than
 * PDN_APRS_MAX_INFO (text received as Latin-1 is written back as UTF-8, up
 * to twice as long), and pdn_aprs_decode_written reads one back. Returns the
 * length, or PDN_APRS_ERR_REFUSED (encoded->reason says why),
 * PDN_APRS_ERR_BUFFER (buf is too small to write and check the field) or
 * PDN_APRS_ERR_ARGUMENT; on failure buf holds nothing of use. options and
 * encoded may be NULL, though a Mic-E report needs encoded for its
 * destination address.
 */
int pdn_aprs_encode_info(const pdn_aprs_data *data, const pdn_aprs_encode_options *options, void *buf,
                         size_t cap, pdn_aprs_encoded *encoded);

/*
 * Encodes a whole TNC2 line "SRC>DEST,PATH:info", NUL-terminated if there
 * is room. For Mic-E the destination is the computed one.
 */
int pdn_aprs_encode_tnc2(const pdn_aprs_header *header, const pdn_aprs_data *data,
                         const pdn_aprs_encode_options *options, char *buf, size_t cap,
                         pdn_aprs_encoded *encoded);

/*
 * Encodes an AX.25 UI frame (addresses, control 0x03, PID 0xF0, information
 * field), without flags or FCS, ready for KISS.
 */
int pdn_aprs_encode_ax25(const pdn_aprs_header *header, const pdn_aprs_data *data,
                         const pdn_aprs_encode_options *options, void *buf, size_t cap,
                         pdn_aprs_encoded *encoded);

/* ------------------------------------------------------------------------ */
/* Building packets                                                         */
/* ------------------------------------------------------------------------ */

/* The sending station, shared by the build calls. */
typedef struct pdn_aprs_station {
    const char *source;      /* "M0LTE-9"; required */
    const char *destination; /* NULL for "APZ001" (experimental software) */
    const char *path;        /* "WIDE1-1,WIDE2-1", or NULL for none */
    uint8_t ax25;            /* 1: write an AX.25 frame; 0: a TNC2 line */
    /* The device table, for checking Mic-E comments (see
       pdn_aprs_encode_options); NULL for none. */
    const pdn_aprs_device_table *devices;
} pdn_aprs_station;

#define PDN_APRS_DEFAULT_DESTINATION "APZ001"

/*
 * A position for a position report, object or item. Set has_ flags for the
 * optional fields you want sent; designated initializers work well:
 *   pdn_aprs_position p = { .latitude = 51.5, .longitude = -0.1,
 *                           .symbol = PDN_APRS_SYMBOL_CAR,
 *                           .has_course = 1, .course_degrees = 88,
 *                           .has_speed = 1, .speed_knots = 36,
 *                           .comment = "Mobile" };
 */
typedef struct pdn_aprs_position {
    double latitude, longitude;
    pdn_aprs_symbol symbol;
    uint8_t ambiguity;   /* 0-4 */
    uint8_t compressed;  /* write the compressed format */
    uint8_t has_course;
    uint16_t course_degrees; /* 1-360; 0 means unknown in some formats */
    uint8_t has_speed;
    double speed_knots;
    uint8_t has_altitude;
    double altitude_feet;
    const pdn_aprs_phg *phg;             /* NULL for none */
    uint8_t has_range;
    double range_miles;
    const pdn_aprs_frequency *frequency; /* NULL for none */
    const pdn_aprs_dao *dao;             /* NULL for none; W and base91 adds precision */
    const pdn_aprs_weather *weather;     /* NULL for none; needs the weather symbol */
    const pdn_aprs_comment_telemetry *telemetry; /* NULL for none */
    const char *comment;                 /* UTF-8, or NULL */
} pdn_aprs_position;

/* A position report: ! or = without a timestamp, / or @ with one. timestamp
   is "DDHHMMz", "HHMMSSh" or NULL; messaging marks the station as able to
   receive messages. Returns the length written, or an error. */
int pdn_aprs_build_position(const pdn_aprs_station *station, const pdn_aprs_position *position,
                            const char *timestamp, int messaging, void *buf, size_t cap);

/* An object (name up to 9 characters, always with a timestamp) or an item
   (name 3-9 characters). killed removes it from maps. */
int pdn_aprs_build_object(const pdn_aprs_station *station, const char *name, const char *timestamp,
                          int killed, const pdn_aprs_position *position, void *buf, size_t cap);
int pdn_aprs_build_item(const pdn_aprs_station *station, const char *name, int killed,
                        const pdn_aprs_position *position, void *buf, size_t cap);

/* A Mic-E report. The destination address is computed and replaces the
   station's; message is a pdn_aprs_mic_e_message (not UNKNOWN). */
typedef struct pdn_aprs_mic_e {
    double latitude, longitude;
    pdn_aprs_symbol symbol;
    uint8_t message;      /* pdn_aprs_mic_e_message */
    uint8_t ambiguity;    /* 0-4 */
    uint8_t has_course;
    uint16_t course_degrees;
    uint8_t has_speed;
    double speed_knots;
    uint8_t has_altitude;
    double altitude_feet;
    char type_code;       /* '`' messaging capable, '\'' not, or 0 */
    const char *device_suffix; /* "_%" etc., or NULL */
    const char *locator;  /* Maidenhead locator, or NULL */
    const pdn_aprs_frequency *frequency;
    const pdn_aprs_dao *dao;
    const pdn_aprs_comment_telemetry *telemetry;
    const char *comment;
} pdn_aprs_mic_e;

int pdn_aprs_build_mic_e(const pdn_aprs_station *station, const pdn_aprs_mic_e *mic_e, void *buf,
                         size_t cap);

/* A positionless weather report _MDHM (timestamp "MMDDHHMM"). For weather
   with a position, use pdn_aprs_build_position with the weather symbol. */
int pdn_aprs_build_weather(const pdn_aprs_station *station, const char *timestamp,
                           const pdn_aprs_weather *weather, void *buf, size_t cap);

/* Initializes weather to no fields; then set values with pdn_aprs_weather_set. */
void pdn_aprs_weather_init(pdn_aprs_weather *weather);
void pdn_aprs_weather_set(pdn_aprs_weather *weather, int index, double value);

/* A message (message_id NULL for none; with one, the addressee should ack). */
int pdn_aprs_build_message(const pdn_aprs_station *station, const char *addressee, const char *text,
                           const char *message_id, void *buf, size_t cap);
int pdn_aprs_build_ack(const pdn_aprs_station *station, const char *addressee, const char *message_id,
                       void *buf, size_t cap);
int pdn_aprs_build_reject(const pdn_aprs_station *station, const char *addressee,
                          const char *message_id, void *buf, size_t cap);

/* A bulletin: id "0"-"9" for a general bulletin, "A"-"Z" for an
   announcement, or a digit and a group name ("4WX"). */
int pdn_aprs_build_bulletin(const pdn_aprs_station *station, const char *id, const char *text,
                            void *buf, size_t cap);

/* A status report, with an optional DHM zulu timestamp "DDHHMMz". */
int pdn_aprs_build_status(const pdn_aprs_station *station, const char *text, const char *timestamp,
                          void *buf, size_t cap);

/* A telemetry report: sequence ("001"), five analog values, eight bits
   ("01101001", B1 first, or NULL) and an optional comment. */
int pdn_aprs_build_telemetry(const pdn_aprs_station *station, const char *sequence,
                             const double analog[5], const char *bits, const char *comment,
                             void *buf, size_t cap);

/* Telemetry metadata, sent as messages addressed to the station whose
   telemetry they describe: up to 13 names or units, up to 15 coefficients,
   and the bit sense with a project title of up to 23 characters. */
int pdn_aprs_build_telemetry_names(const pdn_aprs_station *station, const char *addressee,
                                   const char *const *names, size_t count, void *buf, size_t cap);
int pdn_aprs_build_telemetry_units(const pdn_aprs_station *station, const char *addressee,
                                   const char *const *units, size_t count, void *buf, size_t cap);
int pdn_aprs_build_telemetry_coefficients(const pdn_aprs_station *station, const char *addressee,
                                          const double *coefficients, size_t count, void *buf,
                                          size_t cap);
int pdn_aprs_build_telemetry_bits(const pdn_aprs_station *station, const char *addressee,
                                  const char *bits, const char *project, void *buf, size_t cap);

/* Writes a station and ready-made data as a TNC2 line or AX.25 frame. */
int pdn_aprs_build(const pdn_aprs_station *station, const pdn_aprs_data *data, void *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PDN_APRS_H */
