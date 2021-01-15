#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

struct {
    double t_fract;
    double sum_pulses;
    int n;
} fold[24];

char *graph(double avg_pulses);

int main(int argc, char **argv)
{
    double start, end, t, t_fract, avg_pulses;
    int cnt, idx, pulses, line=0, total_data_points_used=0;
    char s[500];

    if (argc != 3) {
        printf("USAGE: fold <start_time> <end_time>\n");
        exit(1);
    }
    sscanf(argv[1], "%lf", &start);
    sscanf(argv[2], "%lf", &end);
    if (start == 0 || end == 0 || start >= end) {
        printf("USAGE: fold <start_time> <end_time>\n");
        exit(1);
    }

    printf("# start = %f  end = %f\n", start, end);

    while (fgets(s, sizeof(s), stdin)) {
        line++;
        cnt = sscanf(s, "%lf %d", &t, &pulses);
        if (cnt != 2) {
            printf("ERROR: line %d\n", line);
            exit(1);
        }

        if (t < start || t > end) {
            continue;
        }

        t_fract = t - (int)t;
        idx = t_fract * 24;

        if (fold[idx].n == 0) {
            fold[idx].t_fract = t_fract;
        }
        if (fold[idx].t_fract != t_fract) {
            printf("ERROR fold[%d].t_fract = %f is not equal t_fract=%f\n",
                   idx, fold[idx].t_fract, t_fract);
            exit(1);
        }
        fold[idx].sum_pulses += pulses;
        fold[idx].n++;

        total_data_points_used++;
    }

    printf("# total_data_points_used = %d\n", total_data_points_used);

    for (idx = 0; idx < 24; idx++) {
        avg_pulses = fold[idx].sum_pulses / fold[idx].n;
        printf("%8.3f %8.1f   # %s\n",
               fold[idx].t_fract, avg_pulses, graph(avg_pulses));
    }

    return 0;
}

char *graph(double avg_pulses)
{
    static char str[1000];

    int num_stars = nearbyint(avg_pulses / 10);

    if (num_stars >= sizeof(str)-1) {
        printf("ERROR: num_stars %d too large\n", num_stars);
        exit(1);
    }

    memset(str, '*', num_stars);
    str[num_stars] = '\0';

    return str;
}

