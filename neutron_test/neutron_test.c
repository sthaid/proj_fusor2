#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>

#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include "util_mccdaq.h"
#include "util_misc.h"

//
// defines
//

#define VERBOSE_PULSES
//#define VERBOSE_INFO

#define INTVL_US (3600 * (uint64_t)1000000)

#define FILENAME "neutron_test_april4.out"

//
// typedefs
//

//
// variables
//

static int32_t neutron_data_max_pulse = -1;
static bool    sigint_or_sigterm;

//
// prototypes
//

static void signal_handler(int sig);
static char *graph(int32_t pulses);
static int32_t mccdaq_callback(uint16_t * d, int32_t max_d);

// -----------------  MAIN & TOP LEVEL ROUTINES  -------------------------------------

int32_t main(int32_t argc, char **argv)
{
    struct rlimit rl;
    struct sigaction action;
    FILE *fp;
    uint64_t end_us;
    int32_t sum;

    //
    // INIT ...
    //

    // use line bufferring for stdout
    setlinebuf(stdout);

    // allow core dump
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_CORE, &rl);

    // register signal handler for SIGINT, and SIGTERM
    bzero(&action, sizeof(action));
    action.sa_handler = signal_handler;
    sigaction(SIGINT, &action, NULL);
    bzero(&action, sizeof(action));
    action.sa_handler = signal_handler;
    sigaction(SIGTERM, &action, NULL);

    // open output file for appending
    fp = fopen(FILENAME, "a");
    if (fp == NULL) {
        FATAL("failed to open %s for appending, %s\n", FILENAME, strerror(errno));
    }
    setlinebuf(fp);

    // init mccdaq device, used to acquire 500000 samples per second from the
    // ludlum 2929 amplifier output
    mccdaq_init();
    mccdaq_start(mccdaq_callback);

    //
    // RUNTIME ...
    //

    sum = 0;
    end_us = microsec_timer() + INTVL_US;
    while (true) {
        // when a new value of neutron_data_max_pulse is available,
        // sum it up, and reset it back to -1
        while (neutron_data_max_pulse == -1) {
            usleep(1000);
            if (sigint_or_sigterm) goto terminate;
        }
        sum += neutron_data_max_pulse;
        neutron_data_max_pulse = -1;

        // if it has been INTVL_US then print the sum for this interval
        if (microsec_timer() > end_us) {
            time_t t;
            struct tm *tm;
            char str[2000];
            double days;

            t = (get_real_time_us() - INTVL_US/2) / 1000000;
            tm = localtime(&t);
            days = tm->tm_yday +
                   tm->tm_hour / 24. + 
                   tm->tm_min / (24. * 60) + 
                   tm->tm_sec / (24. * 3600);

            snprintf(str, sizeof(str), 
                     "%8.3f %4d   # %02d/%02d/%02d %02d:%02d:%02d - %s", 
                     days, sum,
                     tm->tm_mon+1, tm->tm_mday, tm->tm_year+1900, tm->tm_hour, tm->tm_min, tm->tm_sec, 
                     graph(sum));

            INFO("%s\n", str);
            fprintf(fp, "%s\n", str);

            end_us += INTVL_US;
            sum = 0;
        }
    }

terminate:
    INFO("terminating\n");
    return 0;
}

static void signal_handler(int sig)
{
    sigint_or_sigterm = true;
}

static char *graph(int32_t pulses)
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

// -----------------  MCCDAQ CALLBACK - NEUTRON DETECTOR PULSES  ---------------------

#ifdef VERBOSE_PULSES
static void print_plot_str(int32_t value, int32_t baseline);
#endif

static int32_t mccdaq_callback(uint16_t * d, int32_t max_d)
{
    #define MAX_DATA 1000000

    static int16_t  data[MAX_DATA];
    static int32_t  max_data;
    static int32_t  idx;
    static int32_t  baseline;
    static int32_t  local_max_neutron_pulse;

    #define TUNE_PULSE_THRESHOLD  100 

    #define RESET_FOR_NEXT_SEC \
        do { \
            max_data = 0; \
            idx = 0; \
            local_max_neutron_pulse = 0; \
        } while (0)

    // if max_data too big then 
    //   print an error 
    //   reset 
    // endif
    if (max_data + max_d > MAX_DATA) {
        ERROR("max_data %d or max_d %d are too large\n", max_data, max_d);
        RESET_FOR_NEXT_SEC;
        return 0;
    }

    // copy caller supplied data to static data buffer
    memcpy(data+max_data, d, max_d*sizeof(int16_t));
    max_data += max_d;

    // if we have too little data just return, 
    // until additional data is received
    if (max_data < 100) {
        return 0;
    }

    // search for pulses in the data
    int32_t pulse_start_idx = -1;
    int32_t pulse_end_idx   = -1;
    while (true) {
        // terminate this loop when 
        // - not in-a-pulse and near the end of data OR
        // - at the end of data
        if ((pulse_start_idx == -1 && idx >= max_data-20) || 
            (idx == max_data))
        {
            break;
        }

        // print warning if data out of range
        if (data[idx] > 4095) {
            WARN("data[%d] = %u, is out of range\n", idx, data[idx]);
            data[idx] = 2048;
        }

        // update baseline ...
        // if data[idx] is close to baseline then
        //   baseline is okay
        // else if data[idx+10] is close to baseline then
        //   baseline is okay
        // else if data[idx] and the preceding 3 data values are almost the same then
        //   set baseline to data[idx]
        // endif
        if (pulse_start_idx == -1) {
            if (data[idx] >= baseline-1 && data[idx] <= baseline+1) {
                ;  // okay
            } else if (idx+10 < max_data && data[idx+10] >= baseline-1 && data[idx+10] <= baseline+1) {
                ;  // okay
            } else if ((idx >= 3) &&
                       (data[idx-1] >= data[idx]-1 && data[idx-1] <= data[idx]+1) &&
                       (data[idx-2] >= data[idx]-1 && data[idx-2] <= data[idx]+1) &&
                       (data[idx-3] >= data[idx]-1 && data[idx-3] <= data[idx]+1))
            {
                baseline = data[idx];
            }
        }

        // if baseline has not yet determined then continue
        if (baseline == 0) {
            idx++;
            continue;
        }

        // determine the pulse_start_idx and pulse_end_idx
        if (data[idx] >= (baseline + TUNE_PULSE_THRESHOLD) && pulse_start_idx == -1) {
            pulse_start_idx = idx;
        } else if (pulse_start_idx != -1) {
            if (data[idx] < (baseline + TUNE_PULSE_THRESHOLD)) {
                pulse_end_idx = idx - 1;
            } else if (idx - pulse_start_idx >= 10) {
                WARN("discarding a possible pulse because it's too long, pulse_start_idx=%d\n",
                     pulse_start_idx);
                pulse_start_idx = -1;
                pulse_end_idx = -1;
            }
        }

        // if a pulse has been located ...
        // - increment count of pulses
        // - if verbose print details of the pulse
        // endif
        if (pulse_end_idx != -1) {
            // increment local_max_neutron_pulse
            local_max_neutron_pulse++;

#ifdef VERBOSE_PULSES
            int32_t pulse_height, i;
            int32_t pulse_start_idx_extended, pulse_end_idx_extended;

            // scan from start to end of pulse to determine pulse_height,
            // where pulse_height is the height above the baseline
            pulse_height = -1;
            for (i = pulse_start_idx; i <= pulse_end_idx; i++) {
                if (data[i] - baseline > pulse_height) {
                    pulse_height = data[i] - baseline;
                }
            }
            pulse_height = pulse_height * 10000 / 2048;  // convert to mv

            // print a plot of the pulse, but not more often than every 200 ms
            static uint64_t last_pulse_print_time_us;
            if (microsec_timer() - last_pulse_print_time_us > 200000) {
                pulse_start_idx_extended = pulse_start_idx - 1;
                pulse_end_idx_extended = pulse_end_idx + 4;
                if (pulse_start_idx_extended < 0) {
                    pulse_start_idx_extended = 0;
                }
                if (pulse_end_idx_extended >= max_data) {
                    pulse_end_idx_extended = max_data-1;
                }

                INFO("PULSE:  height_mv = %d   baseline_mv = %d   (%d,%d,%d)\n",
                     pulse_height, (baseline-2048)*10000/2048,
                     pulse_start_idx_extended, pulse_end_idx_extended, max_data);
                for (i = pulse_start_idx_extended; i <= pulse_end_idx_extended; i++) {
                    print_plot_str((data[i]-2048)*10000/2048, (baseline-2048)*10000/2048); 
                }
                BLANK_LINE;

                last_pulse_print_time_us = microsec_timer();
            }
#endif

            // done with this pulse
            pulse_start_idx = -1;
            pulse_end_idx = -1;
        }

        // move to next data 
        idx++;
    }

    // if time has incremented then
    //   - publish new neutron data
    //   - if verbose print info
    //   - reset variables for the next second 
    // endif
    uint64_t time_now = time(NULL);
    static uint64_t time_last_published;
    if (time_now > time_last_published) {    
        int32_t mccdaq_restart_count, baseline_mv;

        // publish new neutron data
        neutron_data_max_pulse = local_max_neutron_pulse;
        time_last_published = time_now;

        // print warning if there are mccdaq_restarts, or other concerns
        mccdaq_restart_count = mccdaq_get_restart_count();
        baseline_mv = (baseline - 2048) * 10000 / 2048;
        if (mccdaq_restart_count != 0 ||
            max_data < 480000 || max_data > 520000 ||
            baseline_mv < 1500 || baseline_mv > 1800)
        {
            WARN("mccdaq_restart_count=%d max_data=%d baseline_mv=%d\n",
                  mccdaq_restart_count, max_data, baseline_mv);
        }

#ifdef VERBOSE_INFO
        // print info, and seperator line 
        INFO("NEUTRON:  neutron_pulse=%d  mccdaq_samples=%d   mccdaq_restarts=%d   baseline_mv=%d\n",
               local_max_neutron_pulse,
               max_data, 
               mccdaq_restart_count, 
               baseline_mv);
        BLANK_LINE;
        INFO("===========================================\n");
        BLANK_LINE;
#endif

        // reset for the next second
        RESET_FOR_NEXT_SEC;
    }

    // return 'continue-scanning' 
    return 0;
}

#ifdef VERBOSE_PULSES
static void print_plot_str(int32_t value, int32_t baseline)
{
    char    str[110];
    int32_t idx, i;

    // args are in mv units

    // value               : expected range 0 - 9995 mv
    // baseline            : expected range 0 - 9995 mv
    // idx = value / 100   : range  0 - 99            

    if (value > 9995) {
        printf("%5d: value is out of range\n", value);
        return;
    }
    if (baseline < 0 || baseline > 9995) {
        printf("%5d: baseline is out of range\n", baseline);
        return;
    }

    if (value < 0) {
        value = 0;
    }

    bzero(str, sizeof(str));

    idx = value / 100;
    for (i = 0; i <= idx; i++) {
        str[i] = '*';
    }

    idx = baseline / 100;
    if (str[idx] == '*') {
        str[idx] = '+';
    } else {
        str[idx] = '|';
        for (i = 0; i < idx; i++) {
            if (str[i] == '\0') {
                str[i] = ' ';
            }
        }
    }

    printf("%5d: %s\n", value, str);
}
#endif
