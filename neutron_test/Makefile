TARGETS = neutron_test 

CC = gcc
OUTPUT_OPTION=-MMD -MP -o $@
CFLAGS = -c -g -O2 -pthread -fsigned-char -Wall 

SRC_NEUTRON_TEST = neutron_test.c \
                   util_misc.c \
                   util_mccdaq.c 
OBJ_NEUTRON_TEST=$(SRC_NEUTRON_TEST:.c=.o)

DEP=$(SRC_NEUTRON_TEST:.c=.d)

MCCDAQ_TEST=

#
# build rules
#

all: $(TARGETS)

ifndef MCCDAQ_TEST
neutron_test: $(OBJ_NEUTRON_TEST) 
	$(CC) -pthread -lrt -lm -o $@ $(OBJ_NEUTRON_TEST) \
            -L/usr/local/lib -lmccusb -lhidapi-libusb -lusb-1.0
	sudo chown root:root $@
	sudo chmod 4777 $@
else
CFLAGS += -DMCCDAQ_TEST
neutron_test: $(OBJ_NEUTRON_TEST) 
	$(CC) -pthread -lrt -lm -o $@ $(OBJ_NEUTRON_TEST) 
endif

-include $(DEP)

#
# clean rule
#

clean:
	rm -f $(TARGETS) $(OBJ_NEUTRON_TEST) $(DEP)

