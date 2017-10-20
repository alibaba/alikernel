MODULE_SOURCE := mod_cpuacct_01.c

ifneq ($(KERNELRELEASE),)
obj-m := $(patsubst %.c,%.o,$(MODULE_SOURCE))
else

SUBDIRS = $(patsubst %/Makefile,%,$(wildcard */Makefile))
SOURCES = $(wildcard ali_*.c) 
OBJECTS = $(patsubst %.c,%.o,$(SOURCES)) 
TARGETS = $(patsubst %.c,%,$(SOURCES))
MODULES = $(patsubst %.c,%.ko,$(MODULE_SOURCE))
LTP_DIR ?= /opt/ltp

all: $(TARGETS) $(MODULES)
	@for dir in $(SUBDIRS); do make -C $$dir $@; done

clean:
	make -C /lib/modules/`uname -r`/build M=`pwd` clean
	rm -f $(OBJECTS) $(TARGETS) $(MODULES)

install: $(TARGETS) $(MODULES)
	install $^ $(LTP_DIR)/testcases/bin
	install ali_sched_debug_*.sh $(LTP_DIR)/testcases/bin

$(TARGETS): $(OBJECTS)
	gcc -o $@ $@.o -lltp -lpthread

$(MODULES): $(MODULE_SOURCE)
	make -C /lib/modules/`uname -r`/build M=`pwd` modules

.c.o:
	gcc -g -c -Wall -o $@ $<

endif
