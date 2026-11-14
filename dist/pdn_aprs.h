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
 *
 * What 1.x promises: source compatibility only. Struct sizes depend on the
 * capacities below (which a program may override with -D), and 1.x may add a
 * field to any struct, so rebuild everything that includes this header when
 * you upgrade, and link the library statically or compile in the single file
 * dist/pdn_aprs.c. Zero-initialise every input struct ("= {0}" or a
 * designated initializer): a field added later means "absent" when zero.
 */
#ifndef PDN_APRS_H
#define PDN_APRS_H

/* Marks the public functions, so a library built from the sources with hidden
   visibility (the CMake default) still exports exactly these. */
#ifndef PDN_APRS_API
#if defined(__GNUC__) || defined(__clang__)
#define PDN_APRS_API __attribute__((visibility("default")))
#else
#define PDN_APRS_API
#endif
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PDN_APRS_VERSION_MAJOR 1
#define PDN_APRS_VERSION_MINOR 1
#define PDN_APRS_VERSION_PATCH 0
#define PDN_APRS_VERSION "1.1.0"

/* The version of the library that was compiled, as "MAJOR.MINOR.PATCH". */
PDN_APRS_API const char *pdn_aprs_version(void);

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
/* The room reject[] in the decode options has for diagnostic codes: fixed, so
   that adding a code never changes the struct. */
#define PDN_APRS_MAX_CODES 128
/* The most diagnostics kept for one packet; any beyond are dropped. */
#define PDN_APRS_MAX_DIAGNOSTICS 24
/* Base-91 comment telemetry and telemetry reports carry up to 5 analog
   channels; telemetry metadata names up to 13 channels and gives up to 15
   equation coefficients. */
#define PDN_APRS_MAX_ANALOG 5
#define PDN_APRS_MAX_META_ITEMS 15
/* Third-party packets nest at most this many deep (vectors interpretation
   "third-party packets nest at most four deep"). */
#define PDN_APRS_MAX_THIRD_PARTY_DEPTH 4
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
#define PDN_APRS_ERR_UNSUPPORTED (-6) /* APRS allows it, but it does not fit this library's fixed buffers */

/* A short plain-ASCII description of a result code. */
PDN_APRS_API const char *pdn_aprs_strerror(int result);

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
PDN_APRS_API const char *pdn_aprs_code_name(int code);
/* The code for a vectors' id, or PDN_APRS_CODE_NONE. */
PDN_APRS_API int pdn_aprs_code_from_name(const char *name, size_t len);
/* 1 if a lenient decoder may tolerate the code with a warning. */
PDN_APRS_API int pdn_aprs_code_tolerable(int code);
/* "info", "warning" or "error". */
PDN_APRS_API const char *pdn_aprs_severity_name(int severity);

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
PDN_APRS_API const char *pdn_aprs_symbol_description(pdn_aprs_symbol symbol);
/* The symbol's name in the named-symbol list ("CAR" for PDN_APRS_SYMBOL_CAR),
   or NULL. */
PDN_APRS_API const char *pdn_aprs_symbol_name(pdn_aprs_symbol symbol);
/* Places an overlay character (0-9 or A-Z) on an alternate-table symbol,
   returning 1 and setting *out; returns 0 if the overlay is not 0-9 or A-Z
   or the symbol is not on the alternate table (or already overlaid). */
PDN_APRS_API int pdn_aprs_symbol_overlay(pdn_aprs_symbol symbol, char overlay, pdn_aprs_symbol *out);
/* Builds a symbol from its table and code characters. */
PDN_APRS_API pdn_aprs_symbol pdn_aprs_symbol_make(char table, char code);

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
PDN_APRS_API const char *pdn_aprs_type_name(int type);

typedef enum pdn_aprs_unrecognized_reason {
    PDN_APRS_UNRECOGNIZED_EMPTY = 0,          /* an empty information field */
    PDN_APRS_UNRECOGNIZED_NOT_APRS,           /* the first byte is not a data type identifier */
    PDN_APRS_UNRECOGNIZED_RESERVED_DATA_TYPE, /* &, + or . */
    PDN_APRS_UNRECOGNIZED_MALFORMED           /* an error stopped decoding; see the diagnostics */
} pdn_aprs_unrecognized_reason;

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
    uint8_t fix;    /* pdn_aprs_fix */
    uint8_t source; /* pdn_aprs_source */
    uint8_t origin; /* pdn_aprs_origin */
} pdn_aprs_compression;
typedef enum pdn_aprs_fix {
    PDN_APRS_FIX_OLD = 0,
    PDN_APRS_FIX_CURRENT = 1
} pdn_aprs_fix;
/* Which NMEA sentence the compressed position came from. */
typedef enum pdn_aprs_source {
    PDN_APRS_SOURCE_OTHER = 0,
    PDN_APRS_SOURCE_GLL = 1,
    PDN_APRS_SOURCE_GGA = 2,
    PDN_APRS_SOURCE_RMC = 3
} pdn_aprs_source;
typedef enum pdn_aprs_origin {
    PDN_APRS_ORIGIN_COMPRESSED = 0,
    PDN_APRS_ORIGIN_TNC_BEACON_TEXT = 1,
    PDN_APRS_ORIGIN_SOFTWARE = 2,
    PDN_APRS_ORIGIN_RESERVED3 = 3,
    PDN_APRS_ORIGIN_KPC3 = 4,
    PDN_APRS_ORIGIN_PICO = 5,
    PDN_APRS_ORIGIN_OTHER_TRACKER = 6,
    PDN_APRS_ORIGIN_DIGIPEATER_CONVERSION = 7
} pdn_aprs_origin;

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
    uint8_t shape;      /* pdn_aprs_area_shape */
    uint8_t color;      /* pdn_aprs_color */
    uint8_t lat_offset; /* yy, as sent */
    uint8_t lon_offset; /* xx, as sent */
    uint8_t has_corridor;
    uint16_t corridor_width_miles; /* {nnn} after a line */
} pdn_aprs_area;
typedef enum pdn_aprs_area_shape {
    PDN_APRS_AREA_OPEN_CIRCLE = 0,
    PDN_APRS_AREA_LINE_DOWN_RIGHT = 1,
    PDN_APRS_AREA_OPEN_ELLIPSE = 2,
    PDN_APRS_AREA_OPEN_TRIANGLE = 3,
    PDN_APRS_AREA_OPEN_BOX = 4,
    PDN_APRS_AREA_FILLED_CIRCLE = 5,
    PDN_APRS_AREA_LINE_DOWN_LEFT = 6,
    PDN_APRS_AREA_FILLED_ELLIPSE = 7,
    PDN_APRS_AREA_FILLED_TRIANGLE = 8,
    PDN_APRS_AREA_FILLED_BOX = 9
} pdn_aprs_area_shape;
/* Colors 0-7 are high intensity, 8-15 the same at low intensity. */
typedef enum pdn_aprs_color {
    PDN_APRS_COLOR_BLACK = 0,
    PDN_APRS_COLOR_BLUE = 1,
    PDN_APRS_COLOR_GREEN = 2,
    PDN_APRS_COLOR_CYAN = 3,
    PDN_APRS_COLOR_RED = 4,
    PDN_APRS_COLOR_VIOLET = 5,
    PDN_APRS_COLOR_YELLOW = 6,
    PDN_APRS_COLOR_GRAY = 7,
    PDN_APRS_COLOR_BLACK_LOW = 8,
    PDN_APRS_COLOR_BLUE_LOW = 9,
    PDN_APRS_COLOR_GREEN_LOW = 10,
    PDN_APRS_COLOR_CYAN_LOW = 11,
    PDN_APRS_COLOR_RED_LOW = 12,
    PDN_APRS_COLOR_VIOLET_LOW = 13,
    PDN_APRS_COLOR_YELLOW_LOW = 14,
    PDN_APRS_COLOR_GRAY_LOW = 15
} pdn_aprs_color;

/* DF report bearing and number/range/quality (APRS12c ch. 7). */
typedef struct pdn_aprs_df_bearing {
    uint16_t bearing_degrees;
    uint8_t number, range, quality;
} pdn_aprs_df_bearing;

/* Storm data (APRS12c ch. 12). */
typedef struct pdn_aprs_storm {
    uint8_t type; /* pdn_aprs_storm_type */
    uint16_t sustained_wind_knots;
    uint16_t gust_knots;
    uint16_t central_pressure_mbar;
    uint16_t hurricane_radius_nm;
    uint16_t tropical_storm_radius_nm;
    uint8_t has_whole_gale_radius;
    uint16_t whole_gale_radius_nm;
} pdn_aprs_storm;
typedef enum pdn_aprs_storm_type {
    PDN_APRS_STORM_TROPICAL_STORM = 0,      /* TS */
    PDN_APRS_STORM_HURRICANE = 1,           /* HC */
    PDN_APRS_STORM_TROPICAL_DEPRESSION = 2  /* TD */
} pdn_aprs_storm_type;

/* !DAO! datum and precision. */
typedef struct pdn_aprs_dao {
    /* 'W' for WGS84, as sent (upper case); or a digit 0-9, a locally
       defined datum, which carries no added precision */
    char datum;
    uint8_t precision; /* pdn_aprs_dao_precision */
} pdn_aprs_dao;
typedef enum pdn_aprs_dao_precision {
    PDN_APRS_DAO_NONE = 0,
    PDN_APRS_DAO_THOUSANDTHS = 1,
    PDN_APRS_DAO_BASE91 = 2
} pdn_aprs_dao_precision;

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
    uint8_t tone;        /* pdn_aprs_tone */
    uint16_t tone_value; /* for TONE, CTCSS and DCS */
    uint8_t has_offset;
    int16_t offset_khz;
    uint8_t has_range;
    uint8_t range;        /* R25m: 25 */
    uint8_t range_km;     /* the range is in km (R25k) */
    uint8_t narrow;       /* lower-case tone letter: narrow modulation */
    uint8_t ten_khz_resolution; /* "146.52 MHz": the frequency to 10 kHz */
} pdn_aprs_frequency;
typedef enum pdn_aprs_tone {
    PDN_APRS_TONE_NONE = 0,
    PDN_APRS_TONE_OFF = 1,
    PDN_APRS_TONE_TONE = 2,
    PDN_APRS_TONE_CTCSS = 3,
    PDN_APRS_TONE_DCS = 4,
    PDN_APRS_TONE_BURST = 5 /* 1750 Hz tone burst */
} pdn_aprs_tone;

/* Weather values, indexed by PDN_APRS_WX_*, each named for its unit. Units are
   as APRS sends them; nothing is converted except a compressed position's
   cs-byte wind speed, which is knots on air and given here in mph. */
typedef enum pdn_aprs_weather_field {
    PDN_APRS_WX_WIND_DIRECTION_DEGREES = 0,
    PDN_APRS_WX_WIND_SPEED_MPH,
    PDN_APRS_WX_WIND_GUST_MPH,
    PDN_APRS_WX_TEMPERATURE_F,
    PDN_APRS_WX_RAIN_1H_IN,
    PDN_APRS_WX_RAIN_24H_IN,
    PDN_APRS_WX_RAIN_MIDNIGHT_IN,
    PDN_APRS_WX_HUMIDITY_PERCENT,
    PDN_APRS_WX_PRESSURE_MBAR,
    PDN_APRS_WX_LUMINOSITY_W_M2,
    PDN_APRS_WX_SNOW_24H_IN,
    PDN_APRS_WX_RAIN_RAW, /* raw rain counter */
    PDN_APRS_WX_COUNT
} pdn_aprs_weather_field;

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
PDN_APRS_API const char *pdn_aprs_weather_field_name(int index);

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
    /* The altitude was sent as a /A= in the text, not in the Mic-E altitude
       bytes (set by the decoder; the encoder writes it the same way). Outside
       the vectors' neutral form. */
    uint8_t altitude_in_comment;
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
    double course_degrees; /* whole degrees when decoded; rounded when encoded */
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
PDN_APRS_API const char *pdn_aprs_telemetry_meta_item(const pdn_aprs_telemetry_meta *meta, unsigned i, size_t *len);

/* A status report (APRS12c ch. 16). */
typedef struct pdn_aprs_status {
    char timestamp[8];      /* DHM zulu, or "" */
    char locator[7];        /* Maidenhead locator, upper case, or "" */
    pdn_aprs_symbol symbol; /* after the locator */
    uint8_t has_beam;
    char beam_heading_code;      /* meteor scatter ^HP: heading code */
    char beam_power_code;        /* power code */
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
    uint8_t format; /* pdn_aprs_raw_weather_format */
    uint16_t data_len;
    char data[PDN_APRS_TEXT_SIZE];
} pdn_aprs_raw_weather;
typedef enum pdn_aprs_raw_weather_format {
    PDN_APRS_RAW_WX_PEET_BROS_HASH = 0,    /* # */
    PDN_APRS_RAW_WX_PEET_BROS_STAR = 1,    /* * */
    PDN_APRS_RAW_WX_ULTIMETER_PACKET = 2,  /* $ULTW */
    PDN_APRS_RAW_WX_ULTIMETER_LOGGING = 3  /* !! */
} pdn_aprs_raw_weather_format;

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
    double radius_miles; /* whole miles when decoded; rounded when encoded */
    /* The footprint's latitude and longitude as sent (" 34.0", "-.1715"),
       any leading space included, for identical re-encoding; "" to format
       the value, and "" too when the text sent is longer than fits here. */
    char latitude_text[24];
    char longitude_text[24];
} pdn_aprs_query;

/* One capability: a token and an optional value, as offsets into the
   capabilities' text. */
typedef struct pdn_aprs_capability {
    uint16_t token_offset, token_length;
    uint8_t has_value;
    uint16_t value_offset, value_length;
} pdn_aprs_capability;

/* Station capabilities: count items, each a token and an optional value,
   all stored in text. */
typedef struct pdn_aprs_capabilities {
    uint8_t count;
    pdn_aprs_capability item[PDN_APRS_MAX_CAPABILITIES];
    char text[PDN_APRS_TEXT_SIZE + 2 * PDN_APRS_MAX_CAPABILITIES];
} pdn_aprs_capabilities;

/* The i-th item's token, or NULL; *len is its length. */
PDN_APRS_API const char *pdn_aprs_capability_token(const pdn_aprs_capabilities *capabilities, unsigned i,
                                                   size_t *len);
/* The i-th item's value, or NULL if it has none; *len is its length. */
PDN_APRS_API const char *pdn_aprs_capability_value(const pdn_aprs_capabilities *capabilities, unsigned i,
                                                   size_t *len);

/* A third-party packet }...: the encapsulated packet in TNC2 form, as
   received. pdn_aprs_decode_third_party() decodes it. Its source may be 1-9
   printable ASCII characters other than > and : (APRS12c ch. 17). A defect
   its header may tolerate (several used markers) is a warning on the inner
   packet, and a strict decode rejects the whole packet as
   invalid-third-party. A q-construct is read only in the outer header, so
   the inner packet's q_construct is always -1. */
typedef struct pdn_aprs_third_party {
    uint16_t len;
    uint8_t packet[2 * PDN_APRS_MAX_INFO]; /* the encoder takes twice what the decoder does, as for text */
    /* Third-party packets this one is inside: 0 for one read from a frame or
       line, set by the decoder. pdn_aprs_decode_third_party reads the inner
       packet one deeper; a third-party packet at depth
       PDN_APRS_MAX_THIRD_PARTY_DEPTH (a fifth level) is not read and is
       unrecognized with invalid-third-party. The encoder ignores it. */
    uint8_t depth;
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
    uint8_t reason; /* for PDN_APRS_TYPE_UNRECOGNIZED: pdn_aprs_unrecognized_reason */
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
    uint8_t reject[PDN_APRS_MAX_CODES];
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
PDN_APRS_API int pdn_aprs_decode_tnc2(const void *line, size_t len, const pdn_aprs_decode_options *options,
                                      pdn_aprs_packet *packet);

/*
 * Decodes an AX.25 UI frame as KISS carries it: addresses, control, PID and
 * information field, without flags or FCS.
 */
PDN_APRS_API int pdn_aprs_decode_ax25(const void *frame, size_t len, const pdn_aprs_decode_options *options,
                                      pdn_aprs_packet *packet);

/*
 * Decodes an information field on its own, with the given header (the
 * destination matters for Mic-E). header may be NULL for N0CALL>APZ001.
 */
PDN_APRS_API int pdn_aprs_decode_info(const pdn_aprs_header *header, const void *info, size_t len,
                                      const pdn_aprs_decode_options *options, pdn_aprs_packet *packet);

/*
 * Decodes an information field as pdn_aprs_decode_info does, but keeps no
 * copy of it in the packet (info_len is 0), so it may be longer than
 * PDN_APRS_MAX_INFO. The encoder writes a field of any length (text received
 * as Latin-1 is written back as UTF-8, up to twice as long), and this reads
 * one back. Returns PDN_APRS_ERR_TOO_LONG, the data unrecognized, if a part
 * of the field is longer than the packet's data holds.
 */
PDN_APRS_API int pdn_aprs_decode_info_unbounded(const pdn_aprs_header *header, const void *info, size_t len,
                                         const pdn_aprs_decode_options *options, pdn_aprs_packet *packet);

/*
 * Decodes the packet inside a third-party packet (data.type
 * PDN_APRS_TYPE_THIRD_PARTY) into inner, which has its own header, data and
 * diagnostics. Third-party packets nest at most PDN_APRS_MAX_THIRD_PARTY_DEPTH
 * (4) deep: the inner packet is one deeper than outer (its depth field), and
 * one whose field starts a fifth } is unrecognized (malformed) with
 * invalid-third-party, so a chain of calls ends there.
 */
PDN_APRS_API int pdn_aprs_decode_third_party(const pdn_aprs_data *outer, const pdn_aprs_decode_options *options,
                                             pdn_aprs_packet *inner);

/* 1 if the packet has a diagnostic of this code (or -1 for any code) and
   severity (or -1 for any). pdn_aprs_has_diagnostic(p, -1, PDN_APRS_SEVERITY_ERROR)
   says whether the packet has errors; with SEVERITY_WARNING, warnings. */
PDN_APRS_API int pdn_aprs_has_diagnostic(const pdn_aprs_packet *packet, int code, int severity);

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
PDN_APRS_API int pdn_aprs_identify_device(const pdn_aprs_device_table *table, const pdn_aprs_packet *packet,
                                          pdn_aprs_device *device);

/* Provided by the optional pdn_aprs_deviceid.c (CC BY-SA 2.0 data). */
PDN_APRS_API const pdn_aprs_device_table *pdn_aprs_devices(void);

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

/* Why the encoder refused: the vectors' refusal reason codes (codes.json,
   "refusals"), in order. Values are fixed once released; new ones are only
   appended, so a caller must allow one it does not know. zero means none.
   pdn_aprs_refusal_name() gives the vectors' kebab-case id. */
typedef enum pdn_aprs_refusal {
    PDN_APRS_REFUSAL_NONE = 0,
    PDN_APRS_REFUSAL_INVALID_ADDRESS,
    PDN_APRS_REFUSAL_AX25_CANNOT_CARRY,
    PDN_APRS_REFUSAL_TNC2_CANNOT_CARRY,
    PDN_APRS_REFUSAL_INVALID_THIRD_PARTY_HEADER,
    PDN_APRS_REFUSAL_INVALID_ADDRESSEE,
    PDN_APRS_REFUSAL_INVALID_NAME,
    PDN_APRS_REFUSAL_INVALID_MESSAGE_ID,
    PDN_APRS_REFUSAL_PADDING_SPACE,
    PDN_APRS_REFUSAL_TOO_LONG,
    PDN_APRS_REFUSAL_MALFORMED,
    PDN_APRS_REFUSAL_INVALID_CHARACTER,
    PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD,
    PDN_APRS_REFUSAL_NOT_A_NUMBER,
    PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE,
    PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM,
    PDN_APRS_REFUSAL_VALUE_GIVEN_TWICE,
    PDN_APRS_REFUSAL_INVALID_TIMESTAMP,
    PDN_APRS_REFUSAL_MISSING_FIELD,
    PDN_APRS_REFUSAL_INVALID_SYMBOL,
    PDN_APRS_REFUSAL_SYMBOL_MISMATCH,
    PDN_APRS_REFUSAL_EXTENSION_CONFLICT,
    PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED,
    PDN_APRS_REFUSAL_INVALID_CODE,
    PDN_APRS_REFUSAL_INVALID_NMEA,
    PDN_APRS_REFUSAL_NOT_ENCODABLE,
    PDN_APRS_REFUSAL_DOES_NOT_READ_BACK
} pdn_aprs_refusal;

/* The vectors' id for a refusal code ("too-long"), or NULL if out of range or
   zero. */
PDN_APRS_API const char *pdn_aprs_refusal_name(int refusal);

typedef struct pdn_aprs_encoded {
    size_t len; /* bytes written */
    /* Mic-E: the destination address the encoder computed ("S32UVT"). */
    char destination[PDN_APRS_ADDR_SIZE];
    /* Why the encoder refused, as plain ASCII, or NULL. */
    const char *reason;
    /* Why, as a pdn_aprs_refusal code the vectors name: zero (none) on
       success and for every error that is not PDN_APRS_ERR_REFUSED. */
    int refusal;
} pdn_aprs_encoded;

/*
 * Encodes data as an information field. Writes at most cap bytes and no NUL.
 * There is no limit on the length but cap: a field may be longer than
 * PDN_APRS_MAX_INFO (text received as Latin-1 is written back as UTF-8, up
 * to twice as long), and pdn_aprs_decode_info_unbounded reads one back. Returns the
 * length, or PDN_APRS_ERR_REFUSED (encoded->reason says why),
 * PDN_APRS_ERR_BUFFER (buf is too small to write and check the field) or
 * PDN_APRS_ERR_ARGUMENT; on failure buf holds nothing of use. options and
 * encoded may be NULL, though a Mic-E report needs encoded for its
 * destination address.
 */
PDN_APRS_API int pdn_aprs_encode_info(const pdn_aprs_data *data, const pdn_aprs_encode_options *options, void *buf,
                                      size_t cap, pdn_aprs_encoded *encoded);

/*
 * Encodes a whole TNC2 line "SRC>DEST,PATH:info", NUL-terminated if there
 * is room. For Mic-E the destination is the computed one.
 */
PDN_APRS_API int pdn_aprs_encode_tnc2(const pdn_aprs_header *header, const pdn_aprs_data *data,
                                      const pdn_aprs_encode_options *options, char *buf, size_t cap,
                                      pdn_aprs_encoded *encoded);

/*
 * Encodes an AX.25 UI frame (addresses, control 0x03, PID 0xF0, information
 * field), without flags or FCS, ready for KISS.
 */
PDN_APRS_API int pdn_aprs_encode_ax25(const pdn_aprs_header *header, const pdn_aprs_data *data,
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
 * Every build call ends with "buf, cap, result". result may be NULL; when it
 * is not, the call fills it: len (the bytes written, as the return value),
 * destination (the destination address, computed for Mic-E) and, on failure,
 * reason, a plain-ASCII sentence that is not part of the stable API (do not
 * match on it), and refusal, the pdn_aprs_refusal code to act on. The calls
 * return the length written, or PDN_APRS_ERR_REFUSED for anything APRS cannot
 * carry, PDN_APRS_ERR_ARGUMENT for a NULL required
 * pointer or other misuse, PDN_APRS_ERR_UNSUPPORTED for text APRS allows but
 * that does not fit the library's fixed buffers, and PDN_APRS_ERR_BUFFER if
 * buf is too small.
 *
 * Every input struct below is zero-initialised by its user ("= {0}" or a
 * designated initializer): zero is "absent" for any field a later 1.x adds.
 */

/*
 * What a position report, an object or an item carries. Set the has_ flags
 * for the optional fields you want sent; designated initializers work well:
 *   pdn_aprs_position_args p = { .latitude = 51.5, .longitude = -0.1,
 *                                .symbol = PDN_APRS_SYMBOL_CAR,
 *                                .has_course = 1, .course_degrees = 88,
 *                                .has_speed = 1, .speed_knots = 36,
 *                                .comment = "Mobile" };
 */
typedef struct pdn_aprs_position_args {
    /* Objects and items: the name (an object's up to 9 characters, an item's
       3 to 9); killed removes it from maps. */
    const char *name;
    uint8_t killed;
    /* Position reports and objects: "DDHHMMz" or "HHMMSSh" (see
       pdn_aprs_format_timestamp), or NULL for none (an object always has
       one). */
    const char *timestamp;
    /* Position reports: the station can receive messages. */
    uint8_t messaging;
    double latitude, longitude;
    pdn_aprs_symbol symbol;
    uint8_t ambiguity;   /* 0-4 */
    uint8_t compressed;  /* write the compressed format */
    uint8_t has_course;
    double course_degrees; /* 360 is north; 0 is unknown (APRS12c ch. 7 and 10) */
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
} pdn_aprs_position_args;

/* A position report: ! or = without a timestamp, / or @ with one. Uses
   everything but name and killed. */
PDN_APRS_API int pdn_aprs_build_position(const pdn_aprs_station *station, const pdn_aprs_position_args *args,
                                         void *buf, size_t cap, pdn_aprs_encoded *result);

/* An object: name and timestamp are set, and killed if it is to be removed
   from maps. */
PDN_APRS_API int pdn_aprs_build_object(const pdn_aprs_station *station, const pdn_aprs_position_args *args,
                                       void *buf, size_t cap, pdn_aprs_encoded *result);

/* An item: name is set (3-9 characters), no timestamp. */
PDN_APRS_API int pdn_aprs_build_item(const pdn_aprs_station *station, const pdn_aprs_position_args *args,
                                     void *buf, size_t cap, pdn_aprs_encoded *result);

/* A Mic-E report. The destination address is computed (result->destination)
   and replaces the station's; mic_e_message is a pdn_aprs_mic_e_message (not
   UNKNOWN). */
typedef struct pdn_aprs_mic_e_args {
    double latitude, longitude;
    pdn_aprs_symbol symbol;
    uint8_t mic_e_message; /* pdn_aprs_mic_e_message */
    /* The station can receive messages: the type code written is ` for set
       and ' for zero. Ignored when type_code names a device type. */
    uint8_t messaging;
    uint8_t ambiguity;    /* 0-4 */
    uint8_t has_course;
    double course_degrees; /* 360 is north; 0 is unknown */
    uint8_t has_speed;
    double speed_knots;
    uint8_t has_altitude;
    double altitude_feet; /* written as a /A= */
    uint8_t has_altitude_m;
    double altitude_m;    /* in the Mic-E altitude bytes, rounded to the metre; not with altitude_feet */
    /* A device type code: '>' or ']' (with device_suffix) or ' '; 0 for none,
       and then messaging picks the type code. */
    char type_code;
    const char *device_suffix; /* "_%" etc., or NULL */
    const char *locator;  /* Maidenhead locator, or NULL */
    const pdn_aprs_frequency *frequency;
    const pdn_aprs_dao *dao;
    const pdn_aprs_comment_telemetry *telemetry;
    const char *comment;
} pdn_aprs_mic_e_args;

PDN_APRS_API int pdn_aprs_build_mic_e(const pdn_aprs_station *station, const pdn_aprs_mic_e_args *args, void *buf,
                                      size_t cap, pdn_aprs_encoded *result);

/* A positionless weather report _MDHM (timestamp "MMDDHHMM"; see
   pdn_aprs_format_timestamp). For weather with a position, use
   pdn_aprs_build_position with the weather symbol. */
PDN_APRS_API int pdn_aprs_build_weather(const pdn_aprs_station *station, const char *timestamp,
                                        const pdn_aprs_weather *weather, void *buf, size_t cap,
                                        pdn_aprs_encoded *result);

/* Initializes weather to no fields; then set values with pdn_aprs_weather_set,
   index being a pdn_aprs_weather_field. */
PDN_APRS_API void pdn_aprs_weather_init(pdn_aprs_weather *weather);
PDN_APRS_API void pdn_aprs_weather_set(pdn_aprs_weather *weather, int index, double value);

/* The formats of pdn_aprs_format_timestamp. */
typedef enum pdn_aprs_timestamp_format {
    PDN_APRS_TIMESTAMP_DHM = 0,  /* "DDHHMMz" */
    PDN_APRS_TIMESTAMP_HMS = 1,  /* "HHMMSSh" */
    PDN_APRS_TIMESTAMP_MDHM = 2  /* "MMDDHHMM" */
} pdn_aprs_timestamp_format;

/* Room for the output of pdn_aprs_format_timestamp: eight characters and a NUL. */
#define PDN_APRS_TIMESTAMP_SIZE 9

/* Writes a UTC time in one of the formats as the builders take it, cutting
   (not rounding) to the format's step. Returns the length (7 or 8), or
   PDN_APRS_ERR_REFUSED for a time that is not a valid one (month 1-12, day
   1-31, hour 0-23, minute 0-59, second 0-59) or PDN_APRS_ERR_ARGUMENT for a
   format that does not exist or a NULL out. The year is not part of any of
   the three formats. */
PDN_APRS_API int pdn_aprs_format_timestamp(int year, int month, int day, int hour, int minute, int second,
                                           int format, char out[PDN_APRS_TIMESTAMP_SIZE]);

/* A message. */
typedef struct pdn_aprs_message_args {
    const char *addressee;
    const char *text;
    /* NULL for none ("" is refused: an ID is 1-5 letters or digits); with
       one, the addressee should ack. */
    const char *message_id;
    /* A reply-ack (APRS12c ch. 14): the ID of a message being acked, NULL for
       none, or "" for a sender that supports reply-acks but has nothing to
       ack. Needs a message_id. */
    const char *reply_ack;
} pdn_aprs_message_args;

PDN_APRS_API int pdn_aprs_build_message(const pdn_aprs_station *station, const pdn_aprs_message_args *args,
                                        void *buf, size_t cap, pdn_aprs_encoded *result);
PDN_APRS_API int pdn_aprs_build_ack(const pdn_aprs_station *station, const char *addressee,
                                    const char *message_id, void *buf, size_t cap, pdn_aprs_encoded *result);
PDN_APRS_API int pdn_aprs_build_reject(const pdn_aprs_station *station, const char *addressee,
                                       const char *message_id, void *buf, size_t cap, pdn_aprs_encoded *result);

/* A bulletin: id '0'-'9' for a general bulletin or 'A'-'Z' for an
   announcement, and for a digit a group of up to 5 characters (the encoder
   refuses one that does not read back; NULL or "" for none). */
PDN_APRS_API int pdn_aprs_build_bulletin(const pdn_aprs_station *station, char id, const char *group,
                                         const char *text, void *buf, size_t cap, pdn_aprs_encoded *result);

/* A status report. timestamp ("DDHHMMz") or locator (4 or 6 characters, with
   symbol after it) may be set, not both; NULL or "" leaves either out.
   beam_heading_code and beam_power_code, when has_beam is set, are the meteor
   scatter ^HP codes after the text. A locator is sent in upper case. */
typedef struct pdn_aprs_status_args {
    const char *text;
    const char *timestamp;
    const char *locator;
    pdn_aprs_symbol symbol;
    uint8_t has_beam;
    char beam_heading_code;
    char beam_power_code;
} pdn_aprs_status_args;

PDN_APRS_API int pdn_aprs_build_status(const pdn_aprs_station *station, const pdn_aprs_status_args *args,
                                       void *buf, size_t cap, pdn_aprs_encoded *result);

/* A telemetry report: a sequence number 0-999 (written with three digits;
   refused above), five analog values, eight bits ("01101001", B1 first, or
   NULL for "00000000") and an optional comment. empty[i] set leaves analog
   channel i empty (sent as nothing between the commas, its analog value
   ignored); zero means present, so a zeroed struct has all five. */
typedef struct pdn_aprs_telemetry_args {
    unsigned sequence;
    double analog[5];
    uint8_t empty[5];
    const char *bits;
    const char *comment;
} pdn_aprs_telemetry_args;

PDN_APRS_API int pdn_aprs_build_telemetry(const pdn_aprs_station *station, const pdn_aprs_telemetry_args *args,
                                          void *buf, size_t cap, pdn_aprs_encoded *result);

/* Telemetry metadata, sent as messages addressed to the station whose
   telemetry they describe (addressee NULL means the station itself): up to 13
   names or units, up to 15 coefficients, and the bit sense with a project
   title of up to 23 characters. */
PDN_APRS_API int pdn_aprs_build_telemetry_names(const pdn_aprs_station *station, const char *addressee,
                                                const char *const *names, size_t count, void *buf, size_t cap,
                                                pdn_aprs_encoded *result);
PDN_APRS_API int pdn_aprs_build_telemetry_units(const pdn_aprs_station *station, const char *addressee,
                                                const char *const *units, size_t count, void *buf, size_t cap,
                                                pdn_aprs_encoded *result);
PDN_APRS_API int pdn_aprs_build_telemetry_coefficients(const pdn_aprs_station *station, const char *addressee,
                                                       const double *coefficients, size_t count, void *buf,
                                                       size_t cap, pdn_aprs_encoded *result);
PDN_APRS_API int pdn_aprs_build_telemetry_bits(const pdn_aprs_station *station, const char *addressee,
                                               const char *bits, const char *project, void *buf, size_t cap,
                                               pdn_aprs_encoded *result);

/* Writes a station and ready-made data as a TNC2 line or AX.25 frame. */
PDN_APRS_API int pdn_aprs_build(const pdn_aprs_station *station, const pdn_aprs_data *data, void *buf,
                                size_t cap, pdn_aprs_encoded *result);

/* ------------------------------------------------------------------------ */
/* Reading a packet                                                         */
/* ------------------------------------------------------------------------ */

/* Writes a decoded packet back as it was received: its header and its
   information field, as a TNC2 line (NUL-terminated if there is room) or as
   an AX.25 UI frame (without flags or FCS). Return the length, or
   PDN_APRS_ERR_HEADER if the packet's header was unusable,
   PDN_APRS_ERR_REFUSED for an address the format cannot carry as sent (an
   AX.25 frame holds no SSID with a leading zero, for one), or
   PDN_APRS_ERR_BUFFER. A packet decoded with pdn_aprs_decode_info_unbounded
   keeps no information field, so only the header is written. */
PDN_APRS_API int pdn_aprs_write_tnc2(const pdn_aprs_packet *packet, char *buf, size_t cap);
PDN_APRS_API int pdn_aprs_write_ax25(const pdn_aprs_packet *packet, void *buf, size_t cap);

/* What a PHG's codes mean: watts (the power code squared), antenna height in
   feet (10 x 2 to the height code), gain in dB, and directivity in degrees
   (45 x the code; -1 for omnidirectional). Any output may be NULL. Returns 1,
   or 0 if phg is NULL. */
PDN_APRS_API int pdn_aprs_phg_values(const pdn_aprs_phg *phg, double *watts, double *height_feet,
                                     double *gain_db, int *directivity_degrees);

#ifdef __cplusplus
}
#endif

#endif /* PDN_APRS_H */
