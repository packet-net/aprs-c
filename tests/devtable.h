/* The device table the tests and tools pass: the optional table, or NULL in a
   build without pdn_aprs_deviceid.c (PDN_APRS_TEST_NO_DEVICEID), where
   device names are the only thing left unchecked. */
#ifndef PDN_APRS_TEST_DEVTABLE_H
#define PDN_APRS_TEST_DEVTABLE_H
#include "pdn_aprs.h"

#ifdef PDN_APRS_TEST_NO_DEVICEID
#define TEST_HAVE_DEVICES 0
static inline const pdn_aprs_device_table *dev_table(void) { return NULL; }
#else
#define TEST_HAVE_DEVICES 1
static inline const pdn_aprs_device_table *dev_table(void) { return pdn_aprs_devices(); }
#endif

#endif
