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
