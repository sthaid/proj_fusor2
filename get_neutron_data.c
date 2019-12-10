/*
Copyright (c) 2019 Steven Haid

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

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

#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include "common.h"
#include "util_mccdaq.h"
#include "util_misc.h"

//
// defines
//

//
// typedefs
//

//
// variables
//

static int32_t         active_thread_count;
static bool            sigint_or_sigterm;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static neutron_data_t  neutron_data;
static uint64_t        neutron_data_time;

//
// prototypes
//

static void signal_handler(int sig);
static void server(void);
static void *server_thread(void * cx);
static int32_t mccdaq_callback(uint16_t * d, int32_t max_d);
static void print_plot_str(int32_t value, int32_t baseline);

// -----------------  MAIN & TOP LEVEL ROUTINES  -------------------------------------

int32_t main(int32_t argc, char **argv)
{
    int32_t wait_ms;
    struct rlimit rl;
    struct sigaction action;

    //
    // INIT ...
    //

    // use line bufferring
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

    // init mccdaq device, used to acquire 500000 samples per second from the
    // ludlum 2929 amplifier output
    mccdaq_init();
    mccdaq_start(mccdaq_callback);

    //
    // RUNTIME ...
    //

    // call server routine which will accept connection from the display program,
    // and send neutron data to the display program every second
    server();

    //
    // TERMINATE ...
    //

    // wait for the server thread to terminate
    for (wait_ms = 0; active_thread_count > 0 && wait_ms < 5000; wait_ms++) {
        usleep(1000);
    }
    if (active_thread_count > 0) {
        ERROR("all threads did not terminate, active_thread_count=%d\n", active_thread_count);
    }
    INFO("terminating\n");
    return 0;
}

static void signal_handler(int sig)
{
    sigint_or_sigterm = true;
}

// -----------------  SERVER  --------------------------------------------------------

static void server(void)
{
    struct sockaddr_in server_address;
    int32_t            listen_sockfd;
    int32_t            ret;
    pthread_t          thread;
    pthread_attr_t     attr;
    int32_t            optval;

    // create socket
    listen_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sockfd == -1) {
        FATAL("socket, %s\n", strerror(errno));
    }

    // set reuseaddr
    optval = 1;
    ret = setsockopt(listen_sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, 4);
    if (ret == -1) {
        FATAL("SO_REUSEADDR, %s\n", strerror(errno));
    }

    // bind socket
    bzero(&server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(PORT);
    ret = bind(listen_sockfd,
               (struct sockaddr *)&server_address,
               sizeof(server_address));
    if (ret == -1) {
        FATAL("bind, %s\n", strerror(errno));
    }

    // listen 
    ret = listen(listen_sockfd, 2);
    if (ret == -1) {
        FATAL("listen, %s\n", strerror(errno));
    }

    // init thread attributes to make thread detached
    if (pthread_attr_init(&attr) != 0) {
        FATAL("pthread_attr_init, %s\n", strerror(errno));
    }
    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
        FATAL("pthread_attr_setdetachstate, %s\n", strerror(errno));
    }

    // loop, accepting connection, and create thread to service the client
    INFO("server: accepting connections\n");
    while (1) {
        int                sockfd;
        socklen_t          len;
        struct sockaddr_in address;

        // accept connection
        len = sizeof(address);
        sockfd = accept(listen_sockfd, (struct sockaddr *) &address, &len);
        if (sockfd == -1) {
            if (sigint_or_sigterm) {
                break;
            }
            FATAL("accept, %s\n", strerror(errno));
        }

        // create thread
        if (pthread_create(&thread, &attr, server_thread, (void*)(uintptr_t)sockfd) != 0) {
            FATAL("pthread_create server_thread, %s\n", strerror(errno));
        }
    }
}

static void * server_thread(void * cx)
{
    int32_t        sockfd;
    uint64_t       last_neutron_data_time;
    neutron_data_t nd;
    ssize_t        len;

    __sync_fetch_and_add(&active_thread_count,1);

    sockfd = (uintptr_t)cx;
    last_neutron_data_time = 0;
    memset(&nd, 0, sizeof(nd));

    INFO("accepted connection\n");

    // loop forever, sending neutron_data to client 
    while (true) {
        // wait for neutron_time to change, and 
        // save the last_neutron_data_time
        while (true) {
            if (neutron_data_time != last_neutron_data_time) {
                last_neutron_data_time = neutron_data_time;
                break;
            }
            usleep(1000);  // 1 ms
            if (sigint_or_sigterm) {
                goto exit_thread;
            }
        }

        // send the neutron data to client
        pthread_mutex_lock(&mutex);
        nd = neutron_data;
        pthread_mutex_unlock(&mutex);
        len = do_send(sockfd, &nd, sizeof(nd));
        if (len != sizeof(nd)) {
            if (len == -1 && (errno == ECONNRESET || errno == EPIPE)) {
                INFO("terminating connection\n");
            } else {
                ERROR("terminating connection - send failed, len=%zd, %s, \n", len, strerror(errno));
            }
            break;
        }
    }

exit_thread:
    // terminate thread
    INFO("terminating connection\n");
    close(sockfd);
    __sync_fetch_and_sub(&active_thread_count,1);
    return NULL;
}

// -----------------  MCCDAQ CALLBACK - NEUTRON DETECTOR PULSES  ---------------------

static int32_t mccdaq_callback(uint16_t * d, int32_t max_d)
{
    #define MAX_DATA 1000000

    static int16_t  data[MAX_DATA];
    static int32_t  max_data;
    static int32_t  idx;
    static int32_t  baseline;
    static int16_t  local_neutron_pulse_data[MAX_NEUTRON_PULSE][MAX_NEUTRON_ADC_PULSE_DATA];
    static int16_t  local_neutron_pulse_mv[MAX_NEUTRON_PULSE];
    static int32_t  local_max_neutron_pulse;
    static uint64_t last_pulse_print_time_us;

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
        // - determine the pulse_height, in mv
        // - save the pulse height in local_neutron_pulse_mv[]
        // - save pulse data in local_neutron_pulse_data
        // - print the pulse to the log file
        // endif
        if (pulse_end_idx != -1) {
            int32_t pulse_height, i, k;
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

            // if there is room to store another neutron pulse then
            // - store the pulse height
            // - store the pulse data
            // endif
            if (local_max_neutron_pulse < MAX_NEUTRON_PULSE) {
                // store pulse height
                local_neutron_pulse_mv[local_max_neutron_pulse] = pulse_height;

                // store pulse data
                pulse_start_idx_extended = pulse_start_idx - (MAX_NEUTRON_ADC_PULSE_DATA/2);
                if (pulse_start_idx_extended < 0) {
                    pulse_start_idx_extended = 0;
                }
                pulse_end_idx_extended = pulse_start_idx_extended + (MAX_NEUTRON_ADC_PULSE_DATA-1);
                if (pulse_end_idx_extended >= max_data) {
                    pulse_end_idx_extended = max_data - 1;
                    pulse_start_idx_extended = pulse_end_idx_extended - (MAX_NEUTRON_ADC_PULSE_DATA-1);
                }
                for (k = 0, i = pulse_start_idx_extended; i <= pulse_end_idx_extended; i++) {
                    local_neutron_pulse_data[local_max_neutron_pulse][k++] =
                        (data[i] - baseline) * 10000 / 2048;    // mv above baseline
                }

                // increment local_max_neutron_pulse
                local_max_neutron_pulse++;
            }

            // print a plot of the pulse, but not more often than every 200 ms
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

            // done with this pulse
            pulse_start_idx = -1;
            pulse_end_idx = -1;
        }

        // move to next data 
        idx++;
    }

    // if time has incremented then
    //   - publish new neutron data
    //   - print to log file
    //   - reset variables for the next second 
    // endif
    uint64_t time_now = time(NULL);
    if (time_now > neutron_data_time) {    
        // publish new neutron data
        pthread_mutex_lock(&mutex);
        neutron_data_time = time_now;
        neutron_data.magic = MAGIC_NEUTRON_DATA;
        neutron_data.max_pulse = local_max_neutron_pulse;
        memcpy(neutron_data.pulse_mv, 
               local_neutron_pulse_mv, 
               local_max_neutron_pulse*sizeof(neutron_data.pulse_mv[0]));
        memcpy(neutron_data.pulse_data, 
               local_neutron_pulse_data, 
               local_max_neutron_pulse*sizeof(neutron_data.pulse_data[0]));
        pthread_mutex_unlock(&mutex);

        // print info, and seperator line 
        INFO("NEUTRON:  neutron_pulse=%d  mccdaq_samples=%d   mccdaq_restarts=%d   baseline_mv=%d\n",
               local_max_neutron_pulse,
               max_data, 
               mccdaq_get_restart_count(), 
               (baseline-2048)*10000/2048);
        BLANK_LINE;
        INFO("===========================================\n");
        BLANK_LINE;

        // reset for the next second
        RESET_FOR_NEXT_SEC;
    }

    // return 'continue-scanning' 
    return 0;
}

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
