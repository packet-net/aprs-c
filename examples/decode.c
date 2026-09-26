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
