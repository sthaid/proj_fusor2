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

#ifndef __COMMON_H__
#define __COMMON_H__

// port for connecting to get_neutron_data
#define PORT 9002

// camera config
#define CAM_WIDTH       640
#define CAM_HEIGHT      480
#define FRAMES_PER_SEC  5

// owon b35 bluetooth meter config
#define OWON_B35_FUSOR_VOLTAGE_METER_ID    0
#define OWON_B35_FUSOR_VOLTAGE_METER_ADDR  "98:84:E3:CD:B8:68"
#define OWON_B35_FUSOR_CURRENT_METER_ID    1
#define OWON_B35_FUSOR_CURRENT_METER_ADDR  "98:84:E3:CD:B8:06"

// dataq adc config
#define DATAQ_ADC_CHAN_PRESSURE  3

// neutron data sent from get_neutron_data to display pgm
#define MAGIC_NEUTRON_DATA          0x5577aacc5577aacc
#define MAX_NEUTRON_PULSE           256 
#define MAX_NEUTRON_ADC_PULSE_DATA  20
typedef struct {
    uint64_t  magic;
    int32_t   max_pulse;
    int32_t   pad;
    int16_t   pulse_mv[MAX_NEUTRON_PULSE];  // store pulse height for each pulse, in mv
    int16_t   pulse_data[MAX_NEUTRON_PULSE][MAX_NEUTRON_ADC_PULSE_DATA];   // mv
} neutron_data_t;

// error codes
#define IS_ERROR(x) ((int32_t)(x) >= ERROR_FIRST && (int32_t)(x) <= ERROR_LAST)
#define ERROR_FIRST                   1000000 
#define ERROR_PRESSURE_SENSOR_FAULTY  1000000
#define ERROR_OVER_PRESSURE           1000001
#define ERROR_NO_VALUE                1000002
#define ERROR_LAST                    1000002
#define ERROR_TEXT(x) \
    ((int32_t)(x) == ERROR_PRESSURE_SENSOR_FAULTY ? "FAULTY" : \
     (int32_t)(x) == ERROR_OVER_PRESSURE          ? "OVERPR" : \
     (int32_t)(x) == ERROR_NO_VALUE               ? "NOVAL"   \
                                                  : "??????")

#endif
