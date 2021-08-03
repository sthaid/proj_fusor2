======================================
PURPOSE
======================================

To determine if the operation of a neutron counters can be
sanity checked by monitoring variations in the background
count rate.

I speculate that background count events are due to neutrons
that orginate from either:
- the sun
- the milky way
- the universe
- the environment

If the events are from the Sun I would expect a sharp drop
off of the count rate at sunset, and for the count rate to 
resume at sunrise. Note, that the 11 year solar cycle is currently at
minimum activity.

If from the Milky Way, the drop off and resumption of the count
rate would be gradual because of the large extent of the Milky Way
across the sky. The period would be about 24 hours. The Milky Way 
is located behind the Sun in December. So, if the counts are from 
the Milky Way, and monitoring in December, the count rate would dip 
to a minimum at night. If measured 3 months later, the time of the 
minimum should shift by 6 hours.

If from the Universe, since the Universe is isotropic the count rate
should remain constant.

If from the environment, such as fluctuations in electric power, or
fluctuations in radon gas concentration, then there is still a possibility
of a 24 hour period; perhaps due to radon gas being expelled by periodic
operation of the furnace.

As of now, I have collected data from 12/18/2020 to 1/4/2021. And there
is an indication of a 24 hour period, with the minimum occurring at about
3AM EST. Refer to RUN_12_18_2020 section below.

======================================
NEUTRON COUNTER
======================================

He3 Neutron Detector Tube -> LUDLUM 2929 -> ADC -> Raspberry Pi

The Ludlum is set to 1700 Volts. The analog output signal
from the Ludlum is sampled by a 500 KHZ ADC. Software running
on the Raspberry Pi scans the ADC data for values that have 
voltage .488 volts above the baseline. These are counted pulses.

The Raspberry Pi software outputs the following:

- neutron_test.out:  a single line, such as this, every 1 hour:
     363.632  404   # 12/29/2020 15:09:24 - ***************************************
  where: - 363.632 is the date/time since the begining of the year,
                   and is equivalent to the 12/29/2020 15:09:24
         - 404 is the number of pulses counted in the previous hour
         - the stars make a graph of the pulses counted per hour

- neutron_test.log:  pulse ADC data is printed to this log file
     12/18/20 13:18:08.555 INFO mccdaq_callback: PULSE:  height_mv = 1420   baseline_mv = 1655   (267763,267770,270000)
     1728: ****************+*
     3076: ****************+**************
     2524: ****************+*********
     2187: ****************+*****
     2138: ****************+*****
     2055: ****************+****
     1982: ****************+***
     1933: ****************+***

======================================
RUN_12_18_2020
======================================

This directory contains results for data collected from 12/18/2020 to 1/4/2021.

The rescale program fixes a graph scaling problem in neutron_test.out, creating
neutron_test.out.rescaled. The neutron_test.out.rescaled file also adds 365 to days
in January, so the day/time field doesn't wrap from 365 back to 0, but instead 
continues increasing from 365. This is needed when supplying this data to the
gnuplot program.

In the run_12_18_2020 directory, the do_gnuplot script plots the entire data range.
It appears that the hardware took about 7 days to stabilize. During this interval the
count rate is gradually increasing from about 50/hour to 300/hour. So, the first 7 days
of data are not included in the analysis described below.

In the fit subdirectory, the do_fit script runs gnuplot and fits a cosine function to the
data. The cosine function is a reasonably good fit with a 24 hour period, and with the minimum 
3.57 hours after midnight.

In the fold subdirectory, the fold.c program calculates hourly average vales. And these
hourly average values are passed to gnuplot, which fits a cosine function. This analysis
shows the minimum to occur 2.92 hours after midnight. This is the approach that I prefer.

======================================
RUN_4_14_2021
======================================

This directory contains results for data collected from 4/4/2021 to 4/27/2021.

The new fit and fold directories are similar to RUN_12_18_2002:
         
                          Time of Minimum
        Technique       12_18_2020      4_14_2021
        ---------       ----------      ---------
        fit               3.57           4.38

        fold              2.92           4.65

Web search has not turned up any references to periodic daily neutron count rate.
