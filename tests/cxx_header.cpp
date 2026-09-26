// Compiles pdn_aprs.h as C++ and calls into the C library, to show the
// header's extern "C" guards and names work for C++ users.
// SPDX-License-Identifier: MIT
#include "pdn_aprs.h"

#include <cstdio>
#include <cstring>

int main()
{
    static pdn_aprs_packet packet;
    const char *line = "M0LTE-9>APZ001,WIDE1-1:!5130.00N/00007.00W>088/036Mobile";
    pdn_aprs_symbol car = PDN_APRS_SYMBOL_CAR;
    if (pdn_aprs_decode_tnc2(line, std::strlen(line), NULL, &packet) != PDN_APRS_OK)
        return 1;
    if (packet.data.type != PDN_APRS_TYPE_POSITION || packet.data.as.report.symbol.code != car.code)
        return 1;
    std::printf("C++: %s at %.4f, %.4f\n", packet.header.source, packet.data.as.report.latitude,
                packet.data.as.report.longitude);
    return 0;
}
