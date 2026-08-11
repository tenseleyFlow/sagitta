ifeq ($(MODE),debug)
CFLAGS += -g
else
CFLAGS += -O2
endif
