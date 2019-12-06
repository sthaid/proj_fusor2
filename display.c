// XXX check this stuff in, and update rpi_data to start the get_neutron_data pgm
// XXX test upstairs with camera, and use get_neutron_data in basement

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

#define _FILE_OFFSET_BITS 64

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
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "common.h"
#include "util_sdl.h"
#include "util_jpeg_decode.h"
#include "util_cam.h"
#include "util_dataq.h"
#include "util_owon_b35.h"
#include "util_misc.h"
#include "about.h"

//
// defines
//

// default values
#define DEFAULT_IMAGE_X                     "320"
#define DEFAULT_IMAGE_Y                     "240"
#define DEFAULT_IMAGE_SIZE                  "300"
#define DEFAULT_NEUTRON_PHT_MV              "100"
#define DEFAULT_NEUTRON_SCALE_CPM           "100"
#define DEFAULT_SUMMARY_GRAPH_TIME_SPAN_SEC "600"
#define DEFAULT_FILE_IDX_PLAYBACK_INIT      "0"
#define DEFAULT_ADC_DATA_GRAPH_SELECT       "0"
#define DEFAULT_ADC_DATA_GRAPH_MAX_Y_MV     "10000"

// mode
enum mode {LIVE, PLAYBACK};
#define MODE_STR(m) ((m) == LIVE ? "LIVE" : "PLAYBACK")

// file
#define MAGIC_FILE        0x1122334455667788
#define MAGIC_DATA_PART1  0xaabbccdd55aa55aa
#define MAGIC_DATA_PART2  0x77777777aaaaaaaa

#define MAX_FILE_DATA_PART1   (12*3600)  // 12 hours
#define MAX_DATA_PART2_LENGTH 1000000

#define FILE_DATA_PART2_OFFSET \
   ((sizeof(file_hdr_t) +  \
     sizeof(struct data_part1_s) * MAX_FILE_DATA_PART1 + \
     0x1000) & ~0xfffL)

// display
#define DEFAULT_WIN_WIDTH  1920
#define DEFAULT_WIN_HEIGHT 1000

#define FONT0_HEIGHT (sdl_font_char_height(0))
#define FONT0_WIDTH  (sdl_font_char_width(0))
#define FONT1_HEIGHT (sdl_font_char_height(1))
#define FONT1_WIDTH  (sdl_font_char_width(1))

#define MAX_ADC_DATA_GRAPH_SELECT 1

// config
#define CONFIG_IMAGE_X                      (config[0].value)
#define CONFIG_IMAGE_Y                      (config[1].value)
#define CONFIG_IMAGE_SIZE                   (config[2].value)
#define CONFIG_NEUTRON_PHT_MV               (config[3].value)
#define CONFIG_NEUTRON_SCALE_CPM            (config[4].value)
#define CONFIG_SUMMARY_GRAPH_TIME_SPAN_SEC  (config[5].value)
#define CONFIG_FILE_IDX_PLAYBACK_INIT       (config[6].value)
#define CONFIG_ADC_DATA_GRAPH_SELECT        (config[7].value)
#define CONFIG_ADC_DATA_GRAPH_MAX_Y_MV      (config[8].value)

// for val2str
#define UNITS_KV     1
#define UNITS_MA     2
#define UNITS_CPM    3
#define UNITS_D2_MT  4
#define UNITS_N2_MT  5

// for get_fusor_pressure
#define GAS_ID_D2 0
#define GAS_ID_N2 1

//
// typedefs
//

typedef struct {
    uint64_t magic;
    uint32_t max;
    uint8_t  reserved[4096-12];
} file_hdr_t;

typedef struct {
    struct data_part1_s {
        uint64_t magic;
        uint64_t time; 

        float    voltage_kv;
        float    current_ma;
        float    d2_pressure_mtorr;
        float    n2_pressure_mtorr;
        int16_t  neutron_pulse_mv[MAX_NEUTRON_PULSE];
        int32_t  max_neutron_pulse;
        int32_t  pad1;

        off_t    data_part2_offset;
        uint32_t data_part2_length;
        uint32_t data_part2_jpeg_buff_len;
    } part1;
    struct data_part2_s {
        uint64_t magic;
        int16_t  neutron_adc_pulse_data[MAX_NEUTRON_PULSE][MAX_NEUTRON_ADC_PULSE_DATA];
        uint8_t  jpeg_buff[0];
    } part2;
} data_t;

//
// variables
//

static uint32_t                 win_width;
static uint32_t                 win_height;
static char                     screenshot_prefix[100];

static enum mode                mode;
static bool                     initial_mode;
static bool                     program_terminating;

static char                     filename[100];
static int32_t                  file_fd;
static file_hdr_t             * file_hdr;
static struct data_part1_s    * file_data_part1;
static int32_t                  file_idx_global;

static bool                     get_live_data_thread_running;

static bool                     cam_thread_running;
static uint8_t                  jpeg_buff[1000000];
static int32_t                  jpeg_buff_len;
static pthread_mutex_t          jpeg_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool                     neutron_data_thread_running;
static char                     neutron_data_server_name[100];
static struct sockaddr_in       neutron_data_server_sockaddr;
static neutron_data_t           neutron_data;
static bool                     neutron_data_avail;
static uint64_t                 neutron_data_time_us;
static pthread_mutex_t          neutron_data_mutex = PTHREAD_MUTEX_INITIALIZER;

static char                     config_path[PATH_MAX];
static const int32_t            config_version = 1;
static config_t                 config[] = { { "image_x",                     DEFAULT_IMAGE_X                     },
                                             { "image_y",                     DEFAULT_IMAGE_Y                     },
                                             { "image_size",                  DEFAULT_IMAGE_SIZE                  },
                                             { "neutron_pht_mv",              DEFAULT_NEUTRON_PHT_MV              },
                                             { "neutron_scale_cpm",           DEFAULT_NEUTRON_SCALE_CPM           },
                                             { "summary_graph_time_span_sec", DEFAULT_SUMMARY_GRAPH_TIME_SPAN_SEC },
                                             { "file_idx_playback_init",      DEFAULT_FILE_IDX_PLAYBACK_INIT      },
                                             { "adc_data_graph_select",       DEFAULT_ADC_DATA_GRAPH_SELECT       },
                                             { "adc_data_graph_max_y_mv",     DEFAULT_ADC_DATA_GRAPH_MAX_Y_MV     },
                                             { "",                            ""                     } };
static int32_t                  image_x;
static int32_t                  image_y;
static int32_t                  image_size;
static int32_t                  neutron_pht_mv;
static int32_t                  neutron_scale_cpm;
static int32_t                  summary_graph_time_span_sec;
static int64_t                  file_idx_playback_init;
static int32_t                  adc_data_graph_select;
static int32_t                  adc_data_graph_max_y_mv;

//
// prototypes
//

static int32_t initialize(int32_t argc, char ** argv);
static void usage(void);
static void atexit_config_write(void);
static int32_t live_init(int32_t argc, char ** argv);
static int32_t playback_init(int32_t argc, char ** argv);
static int32_t init_open_and_map_filename(void);

static void * get_live_data_thread(void * cx);
static int32_t write_data_to_file(data_t * data);
static void * cam_thread(void * cx);
static void * neutron_data_thread(void * cx);
static float get_fusor_voltage_kv(void);
static float get_fusor_current_ma(void);
static void get_fusor_pressure(float *d2_pressure_mtorr, float *n2_pressure_mtorr);
static float convert_adc_pressure(float adc_volts, int32_t gas_id);

static int32_t display_handler();
static void draw_camera_image(rect_t * cam_pane, int32_t file_idx);
static void draw_camera_image_control(char key);
static void draw_data_values(rect_t * data_pane, int32_t file_idx);
static void draw_summary_graph(rect_t * graph_pane, int32_t file_idx);
static void draw_summary_graph_control(char key);
static void draw_adc_data_graph(rect_t * graph_pane, int32_t file_idx);
static void draw_adc_data_graph_control(char key);
static void draw_graph_common(rect_t * graph_pane, char * title_str, int32_t x_range_param, int32_t str_col, char * x_info_str, char * y_info_str, float cursor_pos, char * cursor_str, int32_t max_graph, ...);
static char * val2str(float val, int32_t units);
static struct data_part2_s * read_data_part2(int32_t file_idx);
static float neutron_cpm(int32_t file_idx);

// -----------------  MAIN  ----------------------------------------------------------

int32_t main(int32_t argc, char **argv)
{
    int32_t wait_time_ms;

    // initialize
    if (initialize(argc, argv) < 0) {
        ERROR("initialize failed, program terminating\n");
        return 1;
    }

    // run time
    if (display_handler() < 0) {
        ERROR("display_handler failed, program terminating\n");
        return 1;
    }

    // program termination
    program_terminating = true;
    wait_time_ms = 0;
    while ((get_live_data_thread_running || 
            cam_thread_running || 
            neutron_data_thread_running) && 
           (wait_time_ms < 10000)) 
    {
        usleep(10000);  // 10 ms
        wait_time_ms += 10;
    }

    INFO("terminating normally\n");
    return 0; 
}

// -----------------  INITIALIZE  ----------------------------------------------------

static int32_t initialize(int32_t argc, char ** argv)
{
    struct rlimit rl;
    int32_t       ret;
    const char  * config_dir;

    // use line bufferring
    setlinebuf(stdout);

    // init core dumps
    // note - requires fs.suid_dumpable=1  in /etc/sysctl.conf if this is a suid pgm
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_CORE, &rl);

    // print size of data part1 and part2, and validate sizes are multiple of 8
    // XXX change these info prints to debug
    INFO("sizeof data_t=%zd part1=%zd part2=%zd\n",
          sizeof(data_t), sizeof(struct data_part1_s), sizeof(struct data_part2_s));
    if ((sizeof(struct data_part1_s) % 8) || (sizeof(struct data_part2_s) % 8)) {
        ERROR("sizeof data_t=%zd part1=%zd part2=%zd\n",
              sizeof(data_t), sizeof(struct data_part1_s), sizeof(struct data_part2_s));
        return -1;
    }

    // print sizeof off_t
    // XXX change these info prints to debug
    INFO("sizeof off_t = %zd\n", sizeof(off_t));

    // init globals that are not 0
    mode = LIVE;
    file_idx_global = -1;
    strcpy(neutron_data_server_name, "rpi_data");
    win_width = DEFAULT_WIN_WIDTH;
    win_height = DEFAULT_WIN_HEIGHT;

    // init locals
    strcpy(filename, "");
 
    // parse options
    // -h          : help
    // -g WxH      : window width and height, default 1920x1080
    // -s name     : neutron data server name
    // -p filename : playback file
    while (true) {
        char opt_char = getopt(argc, argv, "hg:s:p:");
        if (opt_char == -1) {
            break;
        }
        switch (opt_char) {
        case 'h':
            usage();
            exit(0);
        case 'g':
            if (sscanf(optarg, "%dx%d", &win_width, &win_height) != 2) {
                ERROR("invalid '-g %s'\n", optarg);
                return -1;
            }
            break;
        case 's':
            strcpy(neutron_data_server_name, optarg);
            break;
        case 'p':
            mode = PLAYBACK;
            strcpy(filename, optarg);
            break;
        default:
            return -1;
        }
    }

    // read config file, and 
    // initialize exit handler to write config file
    config_dir = getenv("HOME");
    if (config_dir == NULL) {
        ERROR("env var HOME not set\n");
        return -1;
    }
    sprintf(config_path, "%s/.fusorrc", config_dir);
    if (config_read(config_path, config, config_version) < 0) {
        ERROR("config_read failed for %s\n", config_path);
        return -1;
    }
    if (sscanf(CONFIG_IMAGE_X, "%d", &image_x) != 1 ||
        sscanf(CONFIG_IMAGE_Y, "%d", &image_y) != 1 ||
        sscanf(CONFIG_IMAGE_SIZE, "%d", &image_size) != 1 ||
        sscanf(CONFIG_NEUTRON_PHT_MV, "%d", &neutron_pht_mv) != 1 ||
        sscanf(CONFIG_NEUTRON_SCALE_CPM, "%d", &neutron_scale_cpm) != 1 ||
        sscanf(CONFIG_SUMMARY_GRAPH_TIME_SPAN_SEC, "%d", &summary_graph_time_span_sec) != 1 ||
        sscanf(CONFIG_FILE_IDX_PLAYBACK_INIT, "%"PRId64, &file_idx_playback_init) != 1 ||
        sscanf(CONFIG_ADC_DATA_GRAPH_SELECT, "%d", &adc_data_graph_select) != 1 ||
        sscanf(CONFIG_ADC_DATA_GRAPH_MAX_Y_MV, "%d", &adc_data_graph_max_y_mv) != 1) 
    {
        ERROR("invalid config value, not a number\n");
        return -1;
    }
    atexit(atexit_config_write);

    // save the initial_mode
    initial_mode = mode;

    // depending on mode, call either live_init or playback_init
    if (mode == LIVE) {
        ret = live_init(argc, argv);
    } else {
        ret = playback_init(argc, argv);
    }
    if (ret != 0) {
        ERROR("%s failed\n", (mode == LIVE ? "live_init" : "playback_init"));
        return -1;
    }

    // return success
    return 0;
}

static void usage(void)
{
    printf("\n"
           "usage: display [options]\n"
           "\n"
           "   where options include:\n"
           "       -h          : help\n"
           "       -g WxH      : window width and height, default 1920x1080\n"
           "       -s name     : neutron data server name\n"
           "       -p filename : playback file\n"
           "\n"
                    );
}

static void atexit_config_write(void)
{
    sprintf(CONFIG_IMAGE_X, "%d", image_x);
    sprintf(CONFIG_IMAGE_Y, "%d", image_y);
    sprintf(CONFIG_IMAGE_SIZE, "%d", image_size);
    sprintf(CONFIG_NEUTRON_PHT_MV, "%d", neutron_pht_mv);
    sprintf(CONFIG_NEUTRON_SCALE_CPM, "%d", neutron_scale_cpm);
    sprintf(CONFIG_SUMMARY_GRAPH_TIME_SPAN_SEC, "%d", summary_graph_time_span_sec);
    if (initial_mode == PLAYBACK) {
        sprintf(CONFIG_FILE_IDX_PLAYBACK_INIT, "%"PRId64, file_idx_global+file_data_part1[0].time);
    } else {
        sprintf(CONFIG_FILE_IDX_PLAYBACK_INIT, "%d", 0);
    }
    sprintf(CONFIG_ADC_DATA_GRAPH_SELECT, "%d", adc_data_graph_select);
    sprintf(CONFIG_ADC_DATA_GRAPH_MAX_Y_MV, "%d", adc_data_graph_max_y_mv);
    config_write(config_path, config, config_version);
}

static int32_t live_init(int32_t argc, char ** argv)
{
    int32_t   len, ret, wait_ms;
    char      s[100];
    pthread_t thread;
    bool      cam_initialized;

    // if filename was provided then 
    //   use the provided filename
    // else 
    //   generate live mode filename
    // endif
    if (argc > optind) {
        strcpy(filename, argv[optind]);
    } else {
        time_t t = time(NULL);
        struct tm * tm = localtime(&t);
        sprintf(filename, "fusor_%2.2d%2.2d%2.2d_%2.2d%2.2d%2.2d.dat",
                tm->tm_year-100, tm->tm_mon+1, tm->tm_mday,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
    INFO("mode                     = %s\n", MODE_STR(mode));
    INFO("filename                 = %s\n", filename);

    // validate filename extension is '.dat'
    len = strlen(filename);
    if (len < 5 || strcmp(&filename[len-4], ".dat") != 0) {
        ERROR("filename must have '.dat. extension\n");
        return -1;
    }

    // save screenshot prefix
    memcpy(screenshot_prefix, filename, len-4);

    // verify filename does not exist
    struct stat stat_buf;
    if (stat(filename, &stat_buf) == 0) {
        ERROR("file %s already exists\n", filename);
        return -1;
    }

    // create the file, and init file hdr
    file_hdr_t hdr;
    int32_t fd;
    fd = open(filename, O_CREAT|O_EXCL|O_RDWR, 0666);
    if (fd < 0) {
        ERROR("failed to create %s, %s\n", filename, strerror(errno));
        return -1;
    }
    bzero(&hdr, sizeof(hdr));
    hdr.magic = MAGIC_FILE;
    hdr.max   = 0;
    if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        ERROR("failed to init %s, %s\n", filename, strerror(errno));
        return -1;
    }
    if (ftruncate(fd,  FILE_DATA_PART2_OFFSET) < 0) {
        ERROR("ftuncate failed on %s, %s\n", filename, strerror(errno));
        return -1;
    }
    close(fd);

    // open and map filename  
    ret = init_open_and_map_filename();
    if (ret != 0) {
        ERROR("init_open_and_map_filename failed\n");
        return -1;
    }

    // verify file header
    if (file_hdr->magic != MAGIC_FILE ||
        file_hdr->max > MAX_FILE_DATA_PART1)
    {
        ERROR("invalid file %s, magic=0x%"PRIx64" max=%d\n", 
              filename, file_hdr->magic, file_hdr->max);
        return -1;
    }

    // get address of server, 
    // print neutron_data_server_name, and serveraddr
    ret =  getsockaddr(neutron_data_server_name, PORT, &neutron_data_server_sockaddr);
    if (ret < 0) {
        ERROR("failed to get address of %s\n", neutron_data_server_name);
        return -1;
    }
    INFO("neutron_data_server_name = %s\n", neutron_data_server_name);
    INFO("neutron_data_serveraddr  = %s\n", 
         sock_addr_to_str(s, sizeof(s), (struct sockaddr *)&neutron_data_server_sockaddr));

    // cam_init
    ret = cam_init(CAM_WIDTH, CAM_HEIGHT, FRAMES_PER_SEC);
    cam_initialized = (ret == 0);
#if 1  // comment out if testing this pgm without a camera
    if (!cam_initialized) {
        ERROR("cam_init failed\n");
        return -1;
    }
#endif
    if (cam_initialized) {
        if (pthread_create(&thread, NULL, cam_thread, NULL) != 0) {
            ERROR("pthread_create cam_thread, %s\n", strerror(errno));
            return -1;
        }
    }

    // create the neutron_data_thread
    if (pthread_create(&thread, NULL, neutron_data_thread, NULL) != 0) {
        ERROR("pthread_create neutron_data_thread, %s\n", strerror(errno));
        return -1;
    }

    // init dataq device used to acquire chamber pressure reading
    dataq_init(0.5,   // averaging duration in secs
               1200,  // scan rate  (samples per second)
               1,     // number of adc channels
               DATAQ_ADC_CHAN_PRESSURE);

    // init owen_b35, used to acquire fusor voltage and current via bluetooth meter
    owon_b35_init(
        2,
        OWON_B35_FUSOR_VOLTAGE_METER_ID, OWON_B35_FUSOR_VOLTAGE_METER_ADDR,
              OWON_B35_VALUE_TYPE_DC_MICROAMP, "voltage",
        OWON_B35_FUSOR_CURRENT_METER_ID, OWON_B35_FUSOR_CURRENT_METER_ADDR,
              OWON_B35_VALUE_TYPE_DC_MILLIAMP, "current"
                        );

    // delay prior to creating get_live_data_thread to give the init routines
    // called above time to create their threads, etc.
    sleep(1);

    // create get_live_data_thread        
    if (pthread_create(&thread, NULL, get_live_data_thread, NULL)) {
        ERROR("pthread_create get_live_data_thread, %s\n", strerror(errno));
        return -1;
    }

    // wait for get_live_data_thread to get first data, tout 5 secs
    wait_ms = 0;
    while (file_idx_global == -1) {
        wait_ms += 10;
        usleep(10000);
        if (wait_ms >= 5000) {
            ERROR("failed to receive data from server\n");
            unlink(filename);
            return -1;
        }
    }

    // return success
    return 0;
}

static int32_t playback_init(int32_t argc, char ** argv)
{
    int32_t len, ret;

    // print filename
    INFO("mode     = %s\n", MODE_STR(mode));
    INFO("filename = %s\n", filename);

    // validate filename extension is '.dat'
    len = strlen(filename);
    if (len < 5 || strcmp(&filename[len-4], ".dat") != 0) {
        ERROR("filename must have '.dat. extension\n");
        return -1;
    }

    // save screenshot prefix
    memcpy(screenshot_prefix, filename, len-4);

    // verify filename exists
    struct stat stat_buf;
    if (stat(filename, &stat_buf) == -1) {
        ERROR("file %s does not exist, %s\n", filename, strerror(errno));
        return -1;
    }
    
    // open and map filename  
    ret = init_open_and_map_filename();
    if (ret != 0) {
        ERROR("init_open_and_map_filename failed\n");
        return -1;
    }

    // verify file header
    if (file_hdr->magic != MAGIC_FILE ||
        file_hdr->max > MAX_FILE_DATA_PART1)
    {
        ERROR("invalid file %s, magic=0x%"PRIx64" max=%d\n", 
              filename, file_hdr->magic, file_hdr->max);
        return -1;
    }

    // verify data_part1 magic
    if (file_data_part1[0].magic != MAGIC_DATA_PART1) {
        ERROR("no data in file %s (0x%"PRIx64"\n", filename, file_data_part1[0].magic);
        return -1;
    }

    // try to init file_idx_global to location from prior invocation of display
    if (file_idx_playback_init != 0 &&
        file_hdr->max >= 1 &&
        file_idx_playback_init >= file_data_part1[0].time &&
        file_idx_playback_init <= file_data_part1[file_hdr->max-1].time)
    {
        file_idx_global = file_idx_playback_init - file_data_part1[0].time;
    } else {
        file_idx_global = 0;
    }

    // sanity check file_idx_global
    if (file_idx_global < 0 || file_idx_global >= file_hdr->max) {
        ERROR("file_idx_global=%d is out of range, file_hdr->max=%d, "
              "setting file_idx_global to zero\n",
              file_idx_global, file_hdr->max);
        file_idx_global = 0;
    }

    // return success
    return 0;
}

static int32_t init_open_and_map_filename(void)
{
    // open filename
    file_fd = open(filename, mode == PLAYBACK ? O_RDONLY : O_RDWR);
    if (file_fd < 0) {
        ERROR("failed to open %s, %s\n", filename, strerror(errno));
        return -1;
    }

    // map file_hdr
    file_hdr = mmap(NULL,  // addr
                    sizeof(file_hdr_t),
                    mode == PLAYBACK ? PROT_READ : PROT_READ|PROT_WRITE,
                    MAP_SHARED,
                    file_fd,
                    0);   // offset
    if (file_hdr == MAP_FAILED) {
        ERROR("failed to map file_hdr %s, %s\n", filename, strerror(errno));
        return -1;
    }

    // map file_data_part1
    file_data_part1 = mmap(NULL,  // addr
                           sizeof(struct data_part1_s) * MAX_FILE_DATA_PART1,
                           mode == PLAYBACK ? PROT_READ : PROT_READ|PROT_WRITE,
                           MAP_SHARED,
                           file_fd,
                           sizeof(file_hdr_t));   // offset
    if (file_data_part1 == MAP_FAILED) {
        ERROR("failed to map file_data_part1 %s, %s\n", filename, strerror(errno));
        return -1;
    }

    // return success
    return 0;
}

// -----------------  GET LIVE DATA THREAD  ------------------------------------------

static void * get_live_data_thread(void * cx)
{
    static char           data_buff[1000000];

    uint64_t              data_time;
    uint64_t              last_time_us;
    data_t              * data;
    struct data_part1_s * dp1;
    struct data_part2_s * dp2;
    neutron_data_t        lnd;

    INFO("starting\n");

    get_live_data_thread_running = true;

    data_time    = time(NULL);
    last_time_us = microsec_timer();
    data         = (data_t *)data_buff;
    dp1          = &data->part1;
    dp2          = &data->part2;
    memset(&lnd, 0, sizeof(lnd));

    while (true) {
        // if program terminating then exit this thread
        if (program_terminating) {
            break;
        }

        // present local-neutron-data varaible to 'no data'
        memset(&lnd, 0, sizeof(lnd));

        // if neutron data has been received in the past 1.9 seconds then
        //   wait for up to 2 secs for next neutron data to be available, and
        //    if available get a copy of the neutron data
        // else
        //   determine interval to sleep to acheive 1 second interval
        // endif
        // increment data_time
        // set last_time_us
        if (microsec_timer() < neutron_data_time_us + 1900000) {
            uint64_t wait_timeout_us = microsec_timer() + 2000000;
            while (!neutron_data_avail && (microsec_timer() < wait_timeout_us)) {
                usleep(1000);
            }
            if (neutron_data_avail) {
                pthread_mutex_lock(&neutron_data_mutex);
                lnd = neutron_data;
                neutron_data_avail = false;
                pthread_mutex_unlock(&neutron_data_mutex);
            }
        } else {
            int64_t sleep_intvl_us = 1000000LL - (microsec_timer() - last_time_us);
            if (sleep_intvl_us > 0) {
                usleep(sleep_intvl_us);
            }
        }
        data_time++;
        last_time_us = microsec_timer();

        // print time deviation
        if ((data_time % 60) == 0) {
            int32_t time_deviation = time(NULL) - data_time;
            INFO("time_deviation = %d secs\n", time_deviation);
        }

        // populate data_t struct
        pthread_mutex_lock(&jpeg_mutex);

        float d2_pressure_mtorr, n2_pressure_mtorr;
        get_fusor_pressure(&d2_pressure_mtorr, &n2_pressure_mtorr);

        dp1->magic                    = MAGIC_DATA_PART1;
        dp1->time                     = data_time;
        dp1->voltage_kv               = get_fusor_voltage_kv();
        dp1->current_ma               = get_fusor_current_ma();
        dp1->d2_pressure_mtorr        = d2_pressure_mtorr;
        dp1->n2_pressure_mtorr        = n2_pressure_mtorr;
        memcpy(dp1->neutron_pulse_mv, lnd.pulse_mv, sizeof(dp1->neutron_pulse_mv));
        dp1->max_neutron_pulse        = lnd.max_pulse;
        dp1->pad1                     = 0;
        dp1->data_part2_offset        = 0;  // filled in by write_data_to_file()
        dp1->data_part2_length        = sizeof(struct data_part2_s) + jpeg_buff_len;
        dp1->data_part2_jpeg_buff_len = jpeg_buff_len;

        dp2->magic                    = MAGIC_DATA_PART2;
        memcpy(dp2->neutron_adc_pulse_data, lnd.pulse_data, sizeof(dp2->neutron_adc_pulse_data));
        memcpy(dp2->jpeg_buff, jpeg_buff, jpeg_buff_len);

        jpeg_buff_len = 0;

        pthread_mutex_unlock(&jpeg_mutex);

        // write data to file
        if (write_data_to_file(data) < 0) {
            FATAL("write_data_to_file failed\n");
        }

        // if live mode then update file_idx_global;
        // note - when the program is started in live mode, this program
        //        can transition in and out of playback mode to view the
        //        earlier data; thus the check here for 'mode == LIVE'
        if (mode == LIVE) {
            file_idx_global = file_hdr->max - 1;
            __sync_synchronize();
        }
    }

    INFO("exitting\n");
    get_live_data_thread_running = false;
    return NULL;
}

static int32_t write_data_to_file(data_t * data)
{
    int32_t         len;

    static uint64_t last_time;
    static off_t    data_part2_offset = FILE_DATA_PART2_OFFSET;

    // if file is full then return error
    if (file_hdr->max >= MAX_FILE_DATA_PART1) {
        ERROR("file is full\n");
        return -1;
    }

    // verify file is being written with increasing timestamp
    if (last_time != 0 && data->part1.time != last_time+1) {
        FATAL("data time out of sequence, %"PRId64" should be %"PRId64"\n",
              data->part1.time, last_time+1);
    }
    last_time = data->part1.time;

    // save file data_part2_offset in data part1
    data->part1.data_part2_offset = data_part2_offset;

    // write data_part1 to file (file_data_part1 is memory mapped to the file)              
    file_data_part1[file_hdr->max] = data->part1;

    // write data_part2 to file
    len = pwrite(file_fd, &data->part2, data->part1.data_part2_length, data_part2_offset);
    if (len != data->part1.data_part2_length) {
        ERROR("write data_part2 len=%d exp=%d, %s\n",
              len, data->part1.data_part2_length, strerror(errno));
        return -1;
    }
    data_part2_offset += data->part1.data_part2_length;

    // update the file_hdr (also memory mapped)
    file_hdr->max++;
    __sync_synchronize();

    // return success
    return 0;
}

// -----------------  GET_LIVE_DATA SUPPORT ROUTINES  --------------------------------

static void * cam_thread(void * cx)
{
    int32_t   ret;
    uint8_t * ptr;
    uint32_t  len;

    INFO("starting\n");
    cam_thread_running = true;

    while (true) {
        // if program terminating then exit this thread
        if (program_terminating) {
            break;
        }

        // get cam buff
        ret = cam_get_buff(&ptr, &len);
        if (ret != 0) {
            usleep(100000);
            continue;
        }

        // copy buff to global
        pthread_mutex_lock(&jpeg_mutex);
        memcpy(jpeg_buff, ptr, len);
        jpeg_buff_len = len;
        pthread_mutex_unlock(&jpeg_mutex);

        // put buff
        cam_put_buff(ptr);
    }

    INFO("exitting\n");
    cam_thread_running = false;
    return NULL;
}

static void * neutron_data_thread(void * cx)
{
    int32_t        sfd;
    int32_t        len;
    struct timeval rcvto;
    neutron_data_t lnd;
    char           s[100];
    int32_t        i;

    INFO("starting\n");
    neutron_data_thread_running = true;

    while (true) {
        // delay 5 secs before attempting to connect
        for (i = 0; i < 5; i++) {
            sleep(1);
            if (program_terminating) {
                break;
            }
        }
        if (program_terminating) {
            break;
        }

        // create socket
        sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == -1) {
            FATAL("create socket, %s\n", strerror(errno));
        }

        // connect to the get_neutron_data server program,
        // if failed to connnect then delay and continue
        if (connect(sfd, 
                    (struct sockaddr *)&neutron_data_server_sockaddr, 
                    sizeof(neutron_data_server_sockaddr)) < 0) 
        {
            ERROR("connect to %s, %s\n", 
                  sock_addr_to_str(s, sizeof(s), (struct sockaddr *)&neutron_data_server_sockaddr),
                  strerror(errno));
            continue;
        }

        // set recv timeout to 5 seconds
        rcvto.tv_sec  = 5;
        rcvto.tv_usec = 0;
        if (setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&rcvto, sizeof(rcvto)) < 0) {
            FATAL("setsockopt SO_RCVTIMEO, %s\n",strerror(errno));
        }

        // loop reading and publishing the neutron data
        while (true) {
            // if program terminating then exit this thread
            if (program_terminating) {
                break;
            }

            // read the neutron_data, if timedout then break
            len = do_recv(sfd, &lnd, sizeof(lnd));
            if (len != sizeof(lnd)) {
                ERROR("recv lnd len=%d exp=%zd, %s\n",
                      len, sizeof(lnd), strerror(errno));
                break;
            }
            if (lnd.magic != MAGIC_NEUTRON_DATA) {
                ERROR("recv lnd bad magic 0x%"PRIx64", exp=0x%"PRIx64"\n", 
                      lnd.magic, MAGIC_NEUTRON_DATA);
                break;
            }

            // publish the neutron_data
            pthread_mutex_lock(&neutron_data_mutex);
            neutron_data = lnd;
            neutron_data_avail = true;
            neutron_data_time_us = microsec_timer();
            pthread_mutex_unlock(&neutron_data_mutex);
        }

        // close the connection
        close(sfd);
    }

    INFO("exitting\n");
    neutron_data_thread_running = false;
    return NULL;
}

static float get_fusor_voltage_kv(void)
{
    double ua, kv;

    // for example:
    //   I = E / R = 1000 v / 10^9 ohm = 10^-6 amps
    // therefore 1uA meter reading means 1kv fusor voltage

    ua = owon_b35_get_value(OWON_B35_FUSOR_VOLTAGE_METER_ID);
    if (ua == ERROR_NO_VALUE) {
        return ERROR_NO_VALUE;
    }

    kv = ua;
    return kv;
}

static float get_fusor_current_ma(void)
{
    double ma;

    ma = owon_b35_get_value(OWON_B35_FUSOR_CURRENT_METER_ID);
    if (ma == ERROR_NO_VALUE) {
        return ERROR_NO_VALUE;
    }

    return ma;
}

static void get_fusor_pressure(float *d2_pressure_mtorr, float *n2_pressure_mtorr)
{
    int32_t ret;
    int16_t mean_mv;

    ret = dataq_get_adc(DATAQ_ADC_CHAN_PRESSURE, NULL, &mean_mv, NULL, NULL, NULL);
    if (ret == 0) {
        *d2_pressure_mtorr = convert_adc_pressure(mean_mv/1000., GAS_ID_D2);
        *n2_pressure_mtorr = convert_adc_pressure(mean_mv/1000., GAS_ID_N2);
    } else {
        *d2_pressure_mtorr = ERROR_NO_VALUE;
        *n2_pressure_mtorr = ERROR_NO_VALUE;
    }
}

// Notes:
// - Refer to http://www.lesker.com/newweb/gauges/pdf/manuals/275iusermanual.pdf
//   section 7.2
// - The gas_tbl below is generated from the table in Section 7.2 of 
//   275iusermanual.pdf. The devel_tools/kjl_275i_log_linear_tbl program
//   converted the table to C code.
static float convert_adc_pressure(float adc_volts, int32_t gas_id)
{
    typedef struct {
        char * name;
        struct {
            float pressure;
            float voltage;
        } interp_tbl[50];
    } gas_t;

    static gas_t gas_tbl[] = { 
        { "D2", // TORRS       VOLTS
          { {     0.00001,     0.000 },
            {     0.00002,     0.301 },
            {     0.00005,     0.699 },
            {     0.0001,      1.000 },
            {     0.0002,      1.301 },
            {     0.0005,      1.699 },
            {     0.0010,      2.114 },
            {     0.0020,      2.380 },
            {     0.0050,      2.778 },
            {     0.0100,      3.083 },
            {     0.0200,      3.386 },
            {     0.0500,      3.778 },
            {     0.1000,      4.083 },
            {     0.2000,      4.398 },
            {     0.5000,      4.837 },
            {     1.0000,      5.190 },
            {     2.0000,      5.616 },
            {     5.0000,      7.391 }, } },
        { "N2",
          { {     0.00001,     0.000 },
            {     0.00002,     0.301 },
            {     0.00005,     0.699 },
            {     0.0001,      1.000 },
            {     0.0002,      1.301 },
            {     0.0005,      1.699 },
            {     0.0010,      2.000 },
            {     0.0020,      2.301 },
            {     0.0050,      2.699 },
            {     0.0100,      3.000 },
            {     0.0200,      3.301 },
            {     0.0500,      3.699 },
            {     0.1000,      4.000 },
            {     0.2000,      4.301 },
            {     0.5000,      4.699 },
            {     1.0000,      5.000 },
            {     2.0000,      5.301 },
            {     5.0000,      5.699 },
            {    10.0000,      6.000 },
            {    20.0000,      6.301 },
            {    50.0000,      6.699 },
            {   100.0000,      7.000 },
            {   200.0000,      7.301 },
            {   300.0000,      7.477 },
            {   400.0000,      7.602 },
            {   500.0000,      7.699 },
            {   600.0000,      7.778 },
            {   700.0000,      7.845 },
            {   760.0000,      7.881 },
            {   800.0000,      7.903 },
            {   900.0000,      7.954 },
            {  1000.0000,      8.000 }, } },
                                                };
    gas_t * gas = &gas_tbl[gas_id];
    int32_t i = 0;

    if (adc_volts < 0.01) {
        return ERROR_PRESSURE_SENSOR_FAULTY;
    }

    while (true) {
        if (gas->interp_tbl[i+1].voltage == 0) {
            return ERROR_OVER_PRESSURE;
        }

        if (adc_volts >= gas->interp_tbl[i].voltage &&
            adc_volts <= gas->interp_tbl[i+1].voltage)
        {
            float p0 = gas->interp_tbl[i].pressure;
            float p1 = gas->interp_tbl[i+1].pressure;
            float v0 = gas->interp_tbl[i].voltage;
            float v1 = gas->interp_tbl[i+1].voltage;
            float torr =  p0 + (p1 - p0) * (adc_volts - v0) / (v1 - v0);
            return torr * 1000.0;
        }
        i++;
    }
}    

// -----------------  DISPLAY HANDLER - MAIN  ----------------------------------------

static int32_t display_handler(void)
{
    #define MAX_PLAYBACK_SPEED 5
    #define SET_PLAYBACK_PAUSED \
        do { \
            playback_speed = 0; \
            playback_advance_us = 0; \
        } while (0)

    bool          quit;
    sdl_event_t * event;
    rect_t        title_pane_full, title_pane; 
    rect_t        cam_pane_full, cam_pane;
    rect_t        data_pane_full, data_pane;
    rect_t        summary_graph_pane_full, summary_graph_pane;
    rect_t        adc_data_graph_pane_full, adc_data_graph_pane;
    char          str[100];
    struct tm   * tm;
    time_t        t;
    int32_t       file_idx;
    int32_t       event_processed_count;
    int32_t       file_max_last;
    int32_t       playback_speed;
    uint64_t      playback_advance_us;

    // initializae 
    quit = false;
    file_max_last = -1;
    playback_speed = 0;
    playback_advance_us = 0;

    if (sdl_init(win_width, win_height, screenshot_prefix) < 0) {
        ERROR("sdl_init %dx%d failed\n", win_width, win_height);
        return -1;
    }

    sdl_get_state(&win_width, &win_height, NULL);

    sdl_init_pane(&title_pane_full, &title_pane, 
                  0, 0, 
                  win_width, FONT0_HEIGHT+4);
    sdl_init_pane(&cam_pane_full, &cam_pane, 
                  0, FONT0_HEIGHT+2, 
                  CAM_HEIGHT+4, CAM_HEIGHT+4); 
    sdl_init_pane(&data_pane_full, &data_pane, 
                  CAM_HEIGHT+2, FONT0_HEIGHT+2, 
                  win_width-(CAM_HEIGHT+2), 2*FONT1_HEIGHT+4); 
    sdl_init_pane(&summary_graph_pane_full, &summary_graph_pane, 
                  0, FONT0_HEIGHT+CAM_HEIGHT+4,
                  win_width, win_height-(FONT0_HEIGHT+CAM_HEIGHT+4));
    sdl_init_pane(&adc_data_graph_pane_full, &adc_data_graph_pane, 
                  CAM_HEIGHT+2,
                  FONT0_HEIGHT+2*FONT1_HEIGHT+4,
                  win_width - (CAM_HEIGHT+2),
                  CAM_HEIGHT+FONT0_HEIGHT+6 - (FONT0_HEIGHT+2*FONT1_HEIGHT+4));

    // loop until quit
    while (!quit) {
        // get the file_idx, and verify
        __sync_synchronize();
        file_idx = file_idx_global;
        __sync_synchronize();
        if (file_idx < 0 ||
            file_idx >= file_hdr->max ||
            file_data_part1[file_idx].magic != MAGIC_DATA_PART1) 
        {
            FATAL("invalid file_idx %d, max =%d\n", file_idx, file_hdr->max);
        }
        DEBUG("file_idx %d\n", file_idx);

        // initialize for display update
        sdl_display_init();

        // draw pane borders   
        sdl_render_pane_border(&title_pane_full, GREEN);
        sdl_render_pane_border(&cam_pane_full,   GREEN);
        sdl_render_pane_border(&data_pane_full,  GREEN);
        sdl_render_pane_border(&summary_graph_pane_full, GREEN);
        sdl_render_pane_border(&adc_data_graph_pane_full, GREEN);

        // draw title line
        // 
        // 0         1         2         3         4         5         6         7        
        // 0123456789 123456789 123456789 123456789 123456789 123456789 123456789 
        // PLAYBACK_PAUSED  yy/mm/dd hh:mm:ss  LOST_CONN    FILE_ERROR   TIME_ERROR    ...  (?) (SHIFT-ESC)
        // ^                ^                  ^            ^            ^                  ^   ^         ^
        // 0                17                 36           49           62                -15  -11       -1

        if (mode == LIVE) {
            sdl_render_text(&title_pane, 0, 0, 0, "LIVE", GREEN, BLACK);
        } else {
            char playback_mode_str[50];
            if (playback_speed == 0) {
                sprintf(playback_mode_str, "PLAYBACK_PAUSED");
            } else {
                sprintf(playback_mode_str, "PLAYBACK_X%d", playback_speed);
            }
            sdl_render_text(&title_pane, 0, 0, 0, playback_mode_str, RED, BLACK);
        }
            
        t = file_data_part1[file_idx].time;
        tm = localtime(&t);
        sprintf(str, "%d/%d/%d %2.2d:%2.2d:%2.2d",
                tm->tm_year-100, tm->tm_mon+1, tm->tm_mday,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
        sdl_render_text(&title_pane, 0, 17, 0, str, WHITE, BLACK);

        sdl_render_text(&title_pane, 0, -11, 0, "(SHIFT-ESC)", WHITE, BLACK);

        sdl_render_text(&title_pane, 0, -15, 0, "(?)", WHITE, BLACK);
        
        // draw the camera image,
        draw_camera_image(&cam_pane, file_idx);

        // draw the data values,
        draw_data_values(&data_pane, file_idx);

        // draw the summary graph
        draw_summary_graph(&summary_graph_pane, file_idx);

        // draw the adc data graph
        draw_adc_data_graph(&adc_data_graph_pane, file_idx);

        // register for events   
        sdl_event_register(SDL_EVENT_KEY_SHIFT_ESC, SDL_EVENT_TYPE_KEY, NULL);       // quit (shift-esc key)
        sdl_event_register('?', SDL_EVENT_TYPE_KEY, NULL);                           // help
        sdl_event_register(SDL_EVENT_KEY_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);      // summary graph cursor 
        sdl_event_register(SDL_EVENT_KEY_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_CTRL_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_CTRL_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL); 
        sdl_event_register(SDL_EVENT_KEY_ALT_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_ALT_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_HOME, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_END, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('+', SDL_EVENT_TYPE_KEY, NULL);                           // summary graph x scale 
        sdl_event_register('=', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('-', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('s', SDL_EVENT_TYPE_KEY, NULL);                           // adc data graph select 
        sdl_event_register('1', SDL_EVENT_TYPE_KEY, NULL);                           // adc data graph y scale
        sdl_event_register('2', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('a', SDL_EVENT_TYPE_KEY, NULL);                           // camera image position & zoom
        sdl_event_register('d', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('w', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('x', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('z', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('Z', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('r', SDL_EVENT_TYPE_KEY, NULL);                             
        sdl_event_register('3', SDL_EVENT_TYPE_KEY, NULL);                           // adjust neutron pulse height thresh
        sdl_event_register('4', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('5', SDL_EVENT_TYPE_KEY, NULL);                           // adjust neutron summary graph scale
        sdl_event_register('6', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('>', SDL_EVENT_TYPE_KEY, NULL);                           // playback speed control
        sdl_event_register('<', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(',', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('.', SDL_EVENT_TYPE_KEY, NULL);

        // present the display
        sdl_display_present();

        // loop until
        // 1- quit flag is set, OR
        // 2- a message has changed state OR
        // 3- (there is no current event) AND
        //    ((at least one event has been processed) OR
        //     (file index that is currently displayed is not file_idx_global) OR
        //     (file max has changed))
        event_processed_count = 0;
        while (true) {
            // get and process event
            event_processed_count++;
            event = sdl_poll_event();
            switch (event->event) {
            case SDL_EVENT_QUIT: case SDL_EVENT_KEY_SHIFT_ESC: 
                quit = true;
                break;
            case SDL_EVENT_SCREENSHOT_TAKEN: {
                // set screen white for 1 second
                rect_t screen_pane, dummy_pane;
                rect_t rect = {0,0,win_width,win_height};
                sdl_init_pane(&screen_pane, &dummy_pane, 
                            0, 0, 
                            win_width, win_height);
                sdl_display_init();
                sdl_render_fill_rect(&screen_pane, &rect, WHITE);
                sdl_display_present();
                usleep(250000);
                break; }
            case '?':  
                sdl_display_text(about);
                SET_PLAYBACK_PAUSED;
                break;
            case SDL_EVENT_KEY_LEFT_ARROW:
            case SDL_EVENT_KEY_CTRL_LEFT_ARROW:
            case SDL_EVENT_KEY_ALT_LEFT_ARROW: {
                int32_t x = file_idx_global;
                x -= (event->event == SDL_EVENT_KEY_LEFT_ARROW      ? 1 :
                      event->event == SDL_EVENT_KEY_CTRL_LEFT_ARROW ? 10
                                                                    : 60);
                if (x < 0) {
                    x = 0;
                }
                file_idx_global = x;
                mode = PLAYBACK;
                SET_PLAYBACK_PAUSED;
                break; }
            case SDL_EVENT_KEY_RIGHT_ARROW:
            case SDL_EVENT_KEY_CTRL_RIGHT_ARROW:
            case SDL_EVENT_KEY_ALT_RIGHT_ARROW: {
                int32_t x = file_idx_global;
                x += (event->event == SDL_EVENT_KEY_RIGHT_ARROW      ? 1 :
                      event->event == SDL_EVENT_KEY_CTRL_RIGHT_ARROW ? 10
                                                                     : 60);
                if (x >= file_hdr->max) {
                    x = file_hdr->max - 1;
                    file_idx_global = x;
                    mode = initial_mode;
                } else {
                    file_idx_global = x;
                    mode = PLAYBACK;
                }
                SET_PLAYBACK_PAUSED;
                break; }
            case SDL_EVENT_KEY_HOME:
                file_idx_global = 0;
                mode = PLAYBACK;
                SET_PLAYBACK_PAUSED;
                break;
            case SDL_EVENT_KEY_END:
                file_idx_global = file_hdr->max - 1;
                mode = initial_mode;
                SET_PLAYBACK_PAUSED;
                break;
            case '-': case '+': case '=':
                draw_summary_graph_control(event->event);
                break;
            case 's': case '1': case '2':
                draw_adc_data_graph_control(event->event);
                break;
            case 'a': case 'd': case 'w': case 'x': case 'z': case 'Z': case 'r':
                draw_camera_image_control(event->event);
                break;
            case '3': case '4': {
                int32_t delta;
                delta = (neutron_pht_mv < 500  ? 1  :
                         neutron_pht_mv < 1000 ? 5  :
                         neutron_pht_mv < 2000 ? 10 : 
                         neutron_pht_mv < 3000 ? 20 : 
                                                 100);
                if (event->event == '3') {
                    neutron_pht_mv -= delta;
                    if (neutron_pht_mv < 0) {
                        neutron_pht_mv = 0;
                    }
                } else if (event->event == '4') {
                    neutron_pht_mv += delta;
                    if (neutron_pht_mv > 10000) {
                        neutron_pht_mv = 10000;
                    }
                } 
                break; }
            case '5': case '6': {
                int32_t delta;
                delta = (neutron_scale_cpm < 500  ? 1  :
                         neutron_scale_cpm < 1000 ? 5  :
                         neutron_scale_cpm < 2000 ? 10 : 
                         neutron_scale_cpm < 3000 ? 20 : 
                                                    100);
                if (event->event == '5') {
                    neutron_scale_cpm -= delta;
                    if (neutron_scale_cpm < 10) {
                        neutron_scale_cpm = 10;
                    }
                } else if (event->event == '6') {
                    neutron_scale_cpm += delta;
                    if (neutron_scale_cpm > 10000) {
                        neutron_scale_cpm = 10000;
                    }
                } 
                break; }
            case '>': case '.':
                if (mode == PLAYBACK && playback_speed < MAX_PLAYBACK_SPEED) {
                    playback_speed++;
                    playback_advance_us = microsec_timer() + 1000000 / playback_speed;
                }
                break;
            case '<': case ',':
                if (mode == PLAYBACK && playback_speed > 0) {
                    playback_speed--;
                    playback_advance_us = (playback_speed ? microsec_timer() + 1000000 / playback_speed : 0);
                }
                break;
            case SDL_EVENT_WIN_MINIMIZED:
                SET_PLAYBACK_PAUSED;
                break;
            case SDL_EVENT_WIN_SIZE_CHANGE:
            case SDL_EVENT_WIN_RESTORED:
                break;
            default:
                event_processed_count--;
                break;
            }

            // if in playback run mode then
            // check if it is time to increment file_idx_global
            if (mode == PLAYBACK && playback_speed > 0) {
                uint64_t curr_us = microsec_timer();
                if (curr_us > playback_advance_us) {
                    int32_t x = file_idx_global+1;
                    if (x >= file_hdr->max) {
                        file_idx_global = file_hdr->max - 1;
                        mode = initial_mode;
                        SET_PLAYBACK_PAUSED;
                    } else {
                        file_idx_global = x;
                        playback_advance_us = curr_us + 1000000 / playback_speed;
                    }
                }
            }

            // test if should break out of this loop
            if ((quit) ||
                ((event->event == SDL_EVENT_NONE) &&
                 ((event_processed_count > 0) ||
                  (file_idx != file_idx_global) ||
                  (file_hdr->max != file_max_last))))
            {
                file_max_last = file_hdr->max;
                break;
            }

            // delay 1 ms
            usleep(1000);
        }
    }

    // return success
    return 0;
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW CAMERA IMAGE  - - - - - - - - - - - - - - 

static void draw_camera_image(rect_t * cam_pane, int32_t file_idx)
{
    uint8_t             * pixel_buff;
    uint32_t              pixel_buff_width;
    uint32_t              pixel_buff_height;
    int32_t               ret;
    struct data_part2_s * data_part2;
    char                * errstr;
    uint32_t              skip_rows, skip_cols;

    static texture_t cam_texture = NULL;
    static int32_t image_size_last = 0;

    // if image_size has changed then reallocate cam_texture
    if (cam_texture == NULL || image_size != image_size_last) {
        sdl_destroy_texture(cam_texture);
        cam_texture = sdl_create_yuy2_texture(image_size,image_size);
        if (cam_texture == NULL) {
            FATAL("failed to create cam_texture\n");
        }
        image_size_last = image_size;
    }

    // if no jpeg buff then 
    //   display 'no image'
    //   return
    // endif
    if (file_data_part1[file_idx].data_part2_jpeg_buff_len == 0 ||
        (data_part2 = read_data_part2(file_idx)) == NULL)
    {
        errstr = "NO IMAGE";
        goto error;
    }
    
    // decode the jpeg buff contained in data_part2
    DEBUG("JPEG BUFF LEN %d\n", file_data_part1[file_idx].data_part2_jpeg_buff_len);
    ret = jpeg_decode(0,  // cxid
                      JPEG_DECODE_MODE_YUY2,      
                      data_part2->jpeg_buff, file_data_part1[file_idx].data_part2_jpeg_buff_len,
                      &pixel_buff, &pixel_buff_width, &pixel_buff_height);
    if (ret < 0) {
        ERROR("jpeg_decode ret %d\n", ret);
        errstr = "DECODE";
        goto error;
    }
    if (pixel_buff_width != CAM_WIDTH || pixel_buff_height != CAM_HEIGHT) {
        ERROR("jpeg_decode wrong dimensions w=%d h=%d\n", pixel_buff_width, pixel_buff_height);
        errstr = "SIZE";
        goto error;
    }

    // display the decoded jpeg
    skip_cols = image_x - image_size/2;
    skip_rows = image_y - image_size/2;
    sdl_update_yuy2_texture(
        cam_texture, 
        pixel_buff + (skip_rows * CAM_WIDTH * 2) + (skip_cols * 2),
        CAM_WIDTH);
    sdl_render_texture(cam_texture, cam_pane);
    free(pixel_buff);

    // return
    return;

    // error  
error:
    sdl_render_text(cam_pane, 2, 1, 1, errstr, WHITE, BLACK);
    return;
}

static void draw_camera_image_control(char key)
{
    // adjust image_x. image_y, image_size
    switch (key) {
    case 'a': 
        image_x += 2;
        break;
    case 'd': 
        image_x -= 2;
        break;
    case 'w': 
        image_y += 2;
        break;
    case 'x': 
        image_y -= 2;
        break;
    case 'z': 
        image_size += 4;
        break;
    case 'Z': 
        image_size -= 4;
        break;
    case 'r':
        image_x    = atoi(DEFAULT_IMAGE_X);
        image_y    = atoi(DEFAULT_IMAGE_Y);
        image_size = atoi(DEFAULT_IMAGE_SIZE);
        break;
    default:
        FATAL("invalid key 0x%x\n", key);
        break;
    }

    // sanitize image_x. image_y, image_size
    if (image_size > CAM_HEIGHT) {
        image_size = CAM_HEIGHT;
    }
    if (image_size < 100) {
        image_size = 100;
    }
    if (image_x - image_size/2 < 0) {
        image_x = image_size/2;
    }
    if (image_x + image_size/2 >= CAM_WIDTH) {
        image_x = CAM_WIDTH - image_size/2;
    }
    if (image_y - image_size/2 < 0) {
        image_y = image_size/2;
    }
    if (image_y + image_size/2 >= CAM_HEIGHT) {
        image_y = CAM_HEIGHT - image_size/2;
    }
    DEBUG("sanitized image_x=%d image_y=%d image_size=%d\n", image_x, image_y, image_size);
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW DATA VALUES  - - - - - - - - - - - - - - 

static void draw_data_values(rect_t * data_pane, int32_t file_idx)
{
    struct data_part1_s * dp1;
    char str[200];

    dp1 = &file_data_part1[file_idx];

    sprintf(str, "%s   %s   %s   NPHT=%d MV",
            val2str(dp1->voltage_kv, UNITS_KV),
            val2str(dp1->current_ma, UNITS_MA),
            val2str(neutron_cpm(file_idx), UNITS_CPM),
            neutron_pht_mv);
    sdl_render_text(data_pane, 0, 0, 1, str, WHITE, BLACK);

    sprintf(str, "%s   %s",
            val2str(dp1->d2_pressure_mtorr, UNITS_D2_MT),
            val2str(dp1->n2_pressure_mtorr, UNITS_N2_MT));
    sdl_render_text(data_pane, 1, 0, 1, str, WHITE, BLACK);
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW SUMMARY GRAPH  - - - - - - - - - - - - 

static void draw_summary_graph(rect_t * graph_pane, int32_t file_idx)
{
    #define MAX_TIME_SPAN  5000

    float    voltage_kv_values[MAX_TIME_SPAN];
    float    current_ma_values[MAX_TIME_SPAN];
    float    d2_pressure_mtorr_values[MAX_TIME_SPAN];
    float    neutron_cpm_values[MAX_TIME_SPAN];
    int32_t  file_idx_start, file_idx_end, max_values, i;
    uint64_t cursor_time_us;
    float    cursor_pos;
    char     x_info_str[100];
    char     cursor_str[100];

    // init x_info_str 
    sprintf(x_info_str, "X: %d SEC", summary_graph_time_span_sec);

    // init file_idx_start & file_idx_end
    if (mode == LIVE) {
        file_idx_end   = file_idx;
        file_idx_start = file_idx_end - (summary_graph_time_span_sec - 1);
    } else {
        file_idx_start = file_idx - summary_graph_time_span_sec / 2;
        file_idx_end   = file_idx_start + summary_graph_time_span_sec - 1;
    }

    // init cursor position and string
    cursor_time_us = file_data_part1[file_idx].time * 1000000;
    cursor_pos = (float)(file_idx - file_idx_start) / (summary_graph_time_span_sec - 1);
    time2str(cursor_str, cursor_time_us, false, false, false);

    // init arrays of the values to graph
    max_values = 0;
    for (i = file_idx_start; i <= file_idx_end; i++) {
        voltage_kv_values[max_values]        = (i >= 0 && i < file_hdr->max)
                                               ? file_data_part1[i].voltage_kv
                                               : ERROR_NO_VALUE;
        current_ma_values[max_values]        = (i >= 0 && i < file_hdr->max)
                                                ? file_data_part1[i].current_ma      
                                                : ERROR_NO_VALUE;
        neutron_cpm_values[max_values]       = (i >= 0 && i < file_hdr->max)
                                                ? neutron_cpm(i)
                                                : ERROR_NO_VALUE;
        d2_pressure_mtorr_values[max_values] = (i >= 0 && i < file_hdr->max)
                                                ? file_data_part1[i].d2_pressure_mtorr
                                                : ERROR_NO_VALUE;
        max_values++;
    }

    // draw the graph
    i = file_idx - file_idx_start;
    double ns = neutron_scale_cpm;
    draw_graph_common(
        graph_pane, 
        "SUMMARY", 
        1200,   
        6, 
        x_info_str, NULL, 
        cursor_pos, cursor_str, 
        4,
        val2str(voltage_kv_values[i],UNITS_KV),           RED,             50., max_values, voltage_kv_values,
        val2str(current_ma_values[i],UNITS_MA),           GREEN,           50., max_values, current_ma_values,
        val2str(neutron_cpm_values[i],UNITS_CPM),         PURPLE,           ns, max_values, neutron_cpm_values,
        val2str(d2_pressure_mtorr_values[i],UNITS_D2_MT), BLUE,           100., max_values, d2_pressure_mtorr_values);
}

#define REDUCE(val, tbl) \
    do { \
        int32_t i; \
        for (i = 1; i < sizeof(tbl)/sizeof(tbl[0]); i++) { \
            if (val <= tbl[i]) { \
                val = tbl[i-1]; \
                break; \
            } \
        } \
    } while (0)
#define INCREASE(val, tbl) \
    do { \
        int32_t i; \
        for (i = sizeof(tbl)/sizeof(tbl[0])-2; i >= 0; i--) { \
            if (val >= tbl[i]) { \
                val = tbl[i+1]; \
                break; \
            } \
        } \
    } while (0)

static void draw_summary_graph_control(char key)
{
    // if changing this, also change MAX_TIME_SPAN above
    //                                       1    5   10    20    30    60  minutes
    static uint32_t time_span_sec_tbl[] = { 60, 300, 600, 1200, 1800, 3600 };

    switch (key) {
    case '-': 
        REDUCE(summary_graph_time_span_sec, time_span_sec_tbl);
        break;
    case '+': case '=':
        INCREASE(summary_graph_time_span_sec, time_span_sec_tbl);
        break;
    default:
        FATAL("invalid key 0x%x\n", key);
        break;
    }
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW ADC DATA GRAPH - - - - - - - - - - - - 

static void draw_adc_data_graph(rect_t * graph_pane, int32_t file_idx)
{
    #define MAX_ADC_DATA  1200

    struct data_part1_s * dp1;
    struct data_part2_s * dp2;
    float adc_data[MAX_ADC_DATA];
    int32_t i, j, k, color;
    int32_t sum=0, cnt=0;
    char title_str[100];

    // init pointer to dp1, and read dp2
    dp1 = &file_data_part1[file_idx];
    dp2 = read_data_part2(file_idx);
    if (dp2 == NULL) {
        ERROR("failed read data part2\n");
    }

    // preset adc_data to NO_VALUE
    for (i = 0; i < MAX_ADC_DATA; i++) {
        adc_data[i] = ERROR_NO_VALUE;
    }

    // init array of the values to graph
    switch (adc_data_graph_select) {
    case 0:
        if (dp2) {
            k = 0;
            for (i = 0; i < dp1->max_neutron_pulse; i++) {
                if (dp1->neutron_pulse_mv[i] >= neutron_pht_mv) {
                    // copy pulse data to adc_data array (to be plotted)
                    for (j = 0; j < MAX_NEUTRON_ADC_PULSE_DATA; j++) {
                        adc_data[k++] = dp2->neutron_adc_pulse_data[i][j];
                        if (k == MAX_ADC_DATA) {
                            break;
                        }
                    }
                    if (k == MAX_ADC_DATA) {
                        break;
                    }

                    // insert gap in adc_data array, between pulses
                    for (j = 0; j < MAX_NEUTRON_ADC_PULSE_DATA; j++) {
                        adc_data[k++] = ERROR_NO_VALUE;
                        if (k == MAX_ADC_DATA) {
                            break;
                        }
                    }
                    if (k == MAX_ADC_DATA) {
                        break;
                    }
                }
            }
        }
        sprintf(title_str, "NEUTRON ADC");
        color = PURPLE;
        break;
    default:
        FATAL("invalid adc_data_graph_select = %d\n", adc_data_graph_select);
        break;
    }

    // append Y scale to title_str
    sprintf(title_str+strlen(title_str), " : Y %d MV", adc_data_graph_max_y_mv);

    // append average to title_str  (if applicable)
    if (cnt > 0) {
        sprintf(title_str+strlen(title_str), " : AVG %d MV", sum / cnt);
    }

    // draw the graph
    draw_graph_common(
        graph_pane,        // the pane
        title_str,         // title_str
        1200,              // x_range
        -8,                // str_col
        "1 SECOND", NULL,  // x_info_str, y_info_str
        -1, NULL,          // cursor_pos, cursor_str
        1,                 // max_graph
        NULL, color, (double)adc_data_graph_max_y_mv, MAX_ADC_DATA, adc_data); 
                           // name,color,y_max,max_values,values
}

static void draw_adc_data_graph_control(char key)
{
    static uint32_t  max_y_mv_tbl[] = {100, 200, 500, 1000, 2000, 5000, 10000};

    switch (key) {
    case 's':
        adc_data_graph_select = ((adc_data_graph_select + 1) % MAX_ADC_DATA_GRAPH_SELECT);
        break;
    case '1':
        REDUCE(adc_data_graph_max_y_mv, max_y_mv_tbl);
        break;
    case '2':
        INCREASE(adc_data_graph_max_y_mv, max_y_mv_tbl);
        break;
    default:
        FATAL("invalid key 0x%x\n", key);
        break;
    }
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW GRAPH COMMON   - - - - - - - - - - - - 

static void draw_graph_common(
    rect_t * graph_pane,   // the pane    
    char * title_str,      // OPTIONAL graph title, displayed on top line centered above x axis
    int32_t x_range_param, // length of the X axis, in pixels
    int32_t str_col,       // controls the loc of the g->name, and x/y_info_str; 0 is at right end of X axis
    char * x_info_str,     // OPTIONAL x axis information
    char * y_info_str,     // OPTIONAL y axis information
    float cursor_pos,      // OPTIONAL cursor position, in X axis pixels;  -1 if not used
    char * cursor_str,     // OPTIONAL string placed under the cursor
    int32_t max_graph,     // number of graphs that follow in the varargs
    ...)
{
    // variable arg list ...
    // - char * name
    // - int32_t color
    // - double  y_max     
    // - int32_t max_values
    // - float * values
    // - ... repeat for next graph ...

    va_list ap;
    rect_t   rect;
    int32_t  i;
    int32_t  gridx;
    int32_t  cursor_x;
    int32_t  title_str_col;
    int32_t  info_str_col;
    int32_t  cursor_str_col;
    int32_t  name_str_col;
    char     name_str[100];
    int32_t  x_origin;
    int32_t  x_range;
    int32_t  y_origin;
    int32_t  y_range;

    struct graph_s {
        char    * name;
        int32_t   color;
        float     y_max;
        int32_t   max_values;
        float   * values;
    } graph[20];

    // get the variable args 
    va_start(ap, max_graph);
    for (i = 0; i < max_graph; i++) {
        graph[i].name       = va_arg(ap, char*);
        graph[i].color      = va_arg(ap, int32_t);
        graph[i].y_max      = va_arg(ap, double);
        graph[i].max_values = va_arg(ap, int32_t);
        graph[i].values     = va_arg(ap, float*);
    }
    va_end(ap);

    x_origin = 10;
    x_range  = x_range_param;
    y_origin = graph_pane->h - FONT0_HEIGHT - 4;
    y_range  = graph_pane->h - FONT0_HEIGHT - 4 - FONT0_HEIGHT;

    // fill white
    rect.x = 0;
    rect.y = 0;
    rect.w = graph_pane->w;
    rect.h = graph_pane->h;
    sdl_render_fill_rect(graph_pane, &rect, WHITE);

    for (gridx = 0; gridx < max_graph; gridx++) {
        #define MAX_POINTS 1000
        #define LINE_WIDTH 2

        struct graph_s * g;
        float            x;
        float            x_pixels_per_val;
        float            y_scale_factor;
        int32_t          y_limit1;
        int32_t          y_limit2;
        int32_t          max_points;

        int32_t          i, lw;
        point_t          points[MAX_POINTS];
        point_t        * p;

        // loop over LINE_WIDTH
        for (lw = 0; lw < LINE_WIDTH; lw++) {
            // init for graph[gridx]
            g                 = &graph[gridx];
            x                 = (float)x_origin;
            x_pixels_per_val  = (float)x_range / g->max_values;
            y_scale_factor    = (float)y_range / g->y_max;
            y_limit1          = y_origin - y_range;
            y_limit2          = y_origin + FONT0_HEIGHT;
            max_points        = 0;

            // draw the graph lines
            for (i = 0; i < g->max_values; i++) {
                if (!IS_ERROR(g->values[i])) {
                    p = &points[max_points];
                    p->x = x;
                    p->y = y_origin - y_scale_factor * g->values[i];
                    if (p->y < y_limit1) {
                        p->y = y_limit1;
                    }
                    if (p->y > y_limit2) {
                        p->y = y_limit2;
                    }
                    p->y += lw;
                    max_points++;
                    if (max_points == MAX_POINTS) {
                        sdl_render_lines(graph_pane, points, max_points, g->color);
                        points[0].x = points[max_points-1].x;
                        points[0].y = points[max_points-1].y;
                        max_points = 1;
                    }
                } else if (max_points > 0) {
                    sdl_render_lines(graph_pane, points, max_points, g->color);
                    max_points = 0;
                }

                x += x_pixels_per_val;
            }
            if (max_points) {
                sdl_render_lines(graph_pane, points, max_points, g->color);
            }
        }

        // draw the graph name
        if (g->name) {
            name_str_col = (x_range + x_origin) / FONT0_WIDTH + str_col;    
            if (g->y_max >= 1) {
                sprintf(name_str, "%-13s  (%.0f MAX)", g->name, g->y_max);
            } else {
                sprintf(name_str, "%-13s  (%.2f MAX)", g->name, g->y_max);
            }
            sdl_render_text(graph_pane, gridx+1, name_str_col, 0, 
                            name_str, g->color, WHITE);
        }
    }

    // draw x axis
    sdl_render_line(graph_pane, 
                    x_origin, y_origin+1, 
                    x_origin+x_range, y_origin+1,
                    BLACK);
    sdl_render_line(graph_pane, 
                    x_origin, y_origin+2, 
                    x_origin+x_range, y_origin+2,
                    BLACK);
    sdl_render_line(graph_pane, 
                    x_origin, y_origin+3, 
                    x_origin+x_range, y_origin+3,
                    BLACK);

    // draw y axis
    sdl_render_line(graph_pane, 
                    x_origin-1, y_origin+3, 
                    x_origin-1, y_origin-y_range,
                    BLACK);
    sdl_render_line(graph_pane, 
                    x_origin-2, y_origin+3, 
                    x_origin-2, y_origin-y_range,
                    BLACK);
    sdl_render_line(graph_pane, 
                    x_origin-3, y_origin+3, 
                    x_origin-3, y_origin-y_range,
                    BLACK);

    // draw cursor, and cursor_str
    if (cursor_pos >= 0) {
        cursor_x = x_origin + cursor_pos * (x_range - 1);
        sdl_render_line(graph_pane,
                        cursor_x-1, y_origin,
                        cursor_x-1, y_origin-y_range,
                        PURPLE);
        sdl_render_line(graph_pane,
                        cursor_x, y_origin,
                        cursor_x, y_origin-y_range,
                        PURPLE);
        if (cursor_str != NULL) {
            cursor_str_col = cursor_x/FONT0_WIDTH - strlen(cursor_str)/2;
            sdl_render_text(graph_pane,
                            -1, cursor_str_col,
                            0, cursor_str, PURPLE, WHITE);
        }
    }

    // draw title_str, and info_str
    if (title_str != NULL) {
        title_str_col = (x_origin + x_range/2) / FONT0_WIDTH - strlen(title_str)/2;
        sdl_render_text(graph_pane,
                        0, title_str_col,
                        0, title_str, BLACK, WHITE);
    }
    if (x_info_str != NULL) {
        info_str_col = (x_range + x_origin) / FONT0_WIDTH + str_col;    
        sdl_render_text(graph_pane,
                        -1, info_str_col,
                        0, x_info_str, BLACK, WHITE);
    }
    if (y_info_str != NULL) {
        info_str_col = (x_range + x_origin) / FONT0_WIDTH + str_col;    
        sdl_render_text(graph_pane,
                        -2, info_str_col,
                        0, y_info_str, BLACK, WHITE);
    }
}

// - - - - - - - - -  DISPLAY HANDLER - SUPPORT ROUTINES  - - - - - - - - - - - - - 

static char * val2str(float val, int32_t units)
{
    static char str_tbl[20][100];
    static uint32_t idx;
    char *str;
    const char *fmt;
    const char *units_str;

    // determine fmt and units_str from units arg
    switch (units) {
    case UNITS_KV:
        units_str = " KV";
        fmt = "%0.1f";
        break;
    case UNITS_MA:
        units_str = " MA";
        fmt = "%0.1f";
        break;
    case UNITS_CPM:
        units_str = " CPM";
        fmt = "%0.1f";
        break;
    case UNITS_D2_MT:
    case UNITS_N2_MT:
        units_str = (units == UNITS_N2_MT ? " N2-MT" : " D2-MT");
        if (IS_ERROR(val)) {
            fmt = "notused";
        } else if (val < 1000) {
            fmt = "%0.1f";
        } else {
            val /= 1000;
            fmt = (val < 10 ? "%0.2fK" : val < 100 ? "%0.1fK" : "%0.0fK");
        }
        break;
    default:
        FATAL("invalid units arg %d\n", units);
        break;
    }

    // pick a static str to use 
    str = str_tbl[idx];
    idx = (idx + 1) % 20;

    // start the return string with the value
    if (IS_ERROR(val)) {
        sprintf(str, "%s", ERROR_TEXT(val));
    } else {
        sprintf(str, fmt, val);
    }

    // append units_str to the end of str
    strcat(str, units_str);

    // return the str
    return str;
}

struct data_part2_s * read_data_part2(int32_t file_idx)
{
    int32_t  dp2_length;
    off_t    dp2_offset;
    int32_t  len;

    static int32_t               last_read_file_idx = -1;
    static struct data_part2_s * last_read_data_part2 = NULL;

    // initial allocate 
    if (last_read_data_part2 == NULL) {
        last_read_data_part2 = calloc(1,MAX_DATA_PART2_LENGTH);
        if (last_read_data_part2 == NULL) {
            FATAL("calloc");
        }
    }

    // if file_idx is same as last read then return data_part2 from last read
    if (file_idx == last_read_file_idx) {
        DEBUG("return cached, file_idx=%d\n", file_idx);
        return last_read_data_part2;
    }

    // verify data_part2 exists for specified file_idx
    if ((dp2_length = file_data_part1[file_idx].data_part2_length) == 0 ||
        (dp2_offset = file_data_part1[file_idx].data_part2_offset) == 0)
    {
        return NULL;
    }

    // read data_part2
    len = pread(file_fd, last_read_data_part2, dp2_length, dp2_offset);
    if (len != dp2_length) {
        ERROR("read data_part2 len=%d exp=%d, %s\n",
              len, dp2_length, strerror(errno));
        return NULL;
    }

    // verify magic value in data_part2
    if (last_read_data_part2->magic != MAGIC_DATA_PART2) {
        FATAL("invalid data_part2 magic 0x%"PRIx64" at file_idx %d\n", 
              last_read_data_part2->magic, file_idx);
    }

    // remember the file_idx of this read, and
    // return the data_part2
    DEBUG("return new read data, file_idx=%d\n", file_idx);
    last_read_file_idx = file_idx;
    return last_read_data_part2;
}

static float neutron_cpm(int32_t file_idx)
{
    #define AVG_SAMPLES 10

    int32_t file_idx_avg_start;
    int32_t file_idx_avg_end;

    static int32_t neutron_pht_mv_cache = -1;
    static int32_t neutron_cps_cache[MAX_FILE_DATA_PART1];
    static float   neutron_cpm_average_cache[MAX_FILE_DATA_PART1];

    // init the start and end of the range to be averaged
    file_idx_avg_start = file_idx - (AVG_SAMPLES-1);
    file_idx_avg_end   = file_idx;

    // if neutron_pht_mv has changed then clear cached results
    if (neutron_pht_mv != neutron_pht_mv_cache) {
        int32_t i;
        for (i = 0; i < MAX_FILE_DATA_PART1; i++) {
            neutron_cps_cache[i] = -1;
            neutron_cpm_average_cache[i] = -1;
        }
        neutron_pht_mv_cache = neutron_pht_mv;
    }

    // if file_idx out of range then return error
    if (file_idx_avg_start < 0 || file_idx_avg_end >= file_hdr->max) {
        return ERROR_NO_VALUE;
    }

    // if cached average cpm value is not available then
    // compute it
    if (neutron_cpm_average_cache[file_idx] == -1) {
        int32_t sum = 0, n = 0, i, j, cps;

        // loop over range to average
        for (i = file_idx_avg_start; i <= file_idx_avg_end; i++) {
            // if cached cps value is not available then compute it
            // based on current setting of neutron_pht_mv
            if (neutron_cps_cache[i] == -1) {
                struct data_part1_s * dp1 = &file_data_part1[i];

                // sanity check dp1->magic
                if (dp1->magic != MAGIC_DATA_PART1) {
                    ERROR("dp1->magix 0x%"PRIx64" is not valid\n", dp1->magic);
                    return ERROR_NO_VALUE;
                }

                // count the number of pulses which have height greater or
                // equal to the pulse-height-threshold
                cps = 0;
                for (j = 0; j < dp1->max_neutron_pulse; j++) {
                    if (dp1->neutron_pulse_mv[j] >= neutron_pht_mv) {
                        cps++;
                    }
                }

                // save the result in the neutron_cps_cache
                neutron_cps_cache[i] = cps;
            }

            // sum the cached cps values
            sum += neutron_cps_cache[i];
            n++;
        }

        // compute cached average cpm value
        neutron_cpm_average_cache[file_idx] = (float)sum / n * 60;
    }

    // return the cached average cpm value
    return neutron_cpm_average_cache[file_idx];
}
