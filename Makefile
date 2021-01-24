TARGETS = display get_neutron_data 

CC = gcc
OUTPUT_OPTION=-MMD -MP -o $@
CFLAGS = -c -g -O2 -pthread -fsigned-char -Wall \
         $(shell sdl2-config --cflags) 

SRC_GET_DATA = get_neutron_data.c \
               util_misc.c \
               util_mccdaq.c 
OBJ_GET_DATA=$(SRC_GET_DATA:.c=.o)

SRC_DISPLAY = display.c \
              util_misc.c \
              util_sdl.c util_sdl_predefined_displays.c \
              util_cam.c util_jpeg_decode.c \
              util_owon_b35.c \
              util_dataq.c 
OBJ_DISPLAY=$(SRC_DISPLAY:.c=.o)

DEP=$(SRC_GET_DATA:.c=.d) $(SRC_DISPLAY:.c=.d)

MCCDAQ_TEST=

#
# build rules
#

all: $(TARGETS)

ifndef MCCDAQ_TEST
get_neutron_data: $(OBJ_GET_DATA) 
	$(CC) -pthread -lrt -lm -o $@ $(OBJ_GET_DATA) \
            -L/usr/local/lib -lmccusb -lhidapi-libusb -lusb-1.0
	sudo chown root:root $@
	sudo chmod 4777 $@
else
CFLAGS += -DMCCDAQ_TEST
get_neutron_data: $(OBJ_GET_DATA) 
	$(CC) -pthread -lrt -lm -o $@ $(OBJ_GET_DATA) 
endif

display: $(OBJ_DISPLAY) 
	$(CC) -Wl,-rpath -Wl,/usr/local/lib \
              -o $@ $(OBJ_DISPLAY) \
              -pthread -lrt -lm -lpng -ljpeg -lSDL2 -lSDL2_ttf -lSDL2_mixer 
              
	sudo chown root:root $@
	sudo chmod 4777 $@

-include $(DEP)

#
# clean rule
#

clean:
	rm -f $(TARGETS) $(OBJ_GET_DATA) $(OBJ_DISPLAY) $(DEP)

