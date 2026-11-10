/* Builds the packets a station sends, as TNC2 lines. */
#include <stdio.h>

#include "pdn_aprs.h"

int main(void)
{
    char line[600];
    int n;
    pdn_aprs_station me = {.source = "M0LTE-9", .path = "WIDE1-1,WIDE2-1"}; /* destination: APZ001 */
    pdn_aprs_encoded result; /* the reason for a refusal; pass NULL if you do not want it */
    pdn_aprs_position_args pos = {.messaging = 1,
                                  .latitude = 51.5,
                                  .longitude = -0.1167,
                                  .symbol = PDN_APRS_SYMBOL_CAR,
                                  .has_course = 1,
                                  .course_degrees = 88,
                                  .has_speed = 1,
                                  .speed_knots = 36,
                                  .comment = "Mobile"};
    pdn_aprs_message_args msg = {.addressee = "G4XYZ", .text = "Meet at the club at 8?", .message_id = "42"};
    pdn_aprs_telemetry_args tlm = {.sequence = 5, .analog = {199, 0, 255, 73, 123}, .bits = "01101001"};

    n = pdn_aprs_build_position(&me, &pos, line, sizeof line, &result);
    if (n < 0) {
        printf("refused: %s\n", result.reason ? result.reason : pdn_aprs_strerror(n));
        return 1;
    }
    printf("%s\n", line);

    n = pdn_aprs_build_message(&me, &msg, line, sizeof line, NULL);
    printf("%s\n", n >= 0 ? line : pdn_aprs_strerror(n));

    n = pdn_aprs_build_telemetry(&me, &tlm, line, sizeof line, NULL);
    printf("%s\n", n >= 0 ? line : pdn_aprs_strerror(n));

    /* A message over 67 characters is refused, not truncated. */
    msg.text = "This text is far too long for an APRS message, which carries 67 characters.";
    msg.message_id = NULL;
    n = pdn_aprs_build_message(&me, &msg, line, sizeof line, &result);
    printf("long message: %s\n", result.reason ? result.reason : pdn_aprs_strerror(n));
    return 0;
}
