// gcc -Wall -Werror -lm -o rescale rescale.c
//
// regenerate the graph using a new scaling XXX comments on year wrap too
// 
// input file line:  "358.882  408   # 12/24/120 21:09:24 - *********"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

char *graph(int pulses);

int main()
{
    char s[1000];
    int pulses, cnt;
    double days;

    while (fgets(s, sizeof(s), stdin) != NULL) {
        cnt = sscanf(s, "%lf %d", &days, &pulses);
        s[38] = 0;
        if (cnt != 2) {
            printf("ERROR: '%s\n", s);
            exit(1);
        }

        if (days < 20) {
            char new_days[100];
            sprintf(new_days, "%8.3f", days+366);
            memcpy(s, new_days, 8);
        }

        printf("%s %s\n", s, graph(pulses));
    }

    return 0;
}

char *graph(int pulses)
{
    static char str[1000];
    int num_stars;

    num_stars = nearbyint(pulses / 10.);

    if (num_stars >= sizeof(str)-1) {
        printf("ERROR: pulses %d too large\n", pulses);
        exit(1);
    }

    memset(str, '*', num_stars);
    str[num_stars] = '\0';

    return str;
}

