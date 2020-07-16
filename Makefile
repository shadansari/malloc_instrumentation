CC = gcc
CFLAGS = -fPIC -Wall -O2 -g
LDFLAGS = -shared
LIBS = -lm -ldl
RM = rm -f
MALLOC_INSTRUMENT_LIB = malloc_instrument.so

SRCS = malloc_instrument.c
OBJS = $(SRCS:.c=.o)

$(MALLOC_INSTRUMENT_LIB): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(SRCS:.c=.d):%.d:%.c
	$(CC) $(CFLAGS) -MM $< >$@

#include $(SRCS:.c=.d)

test: test-clean $(MALLOC_INSTRUMENT_LIB)
	cd test && $(MAKE) clean && $(MAKE)
	LD_PRELOAD=./malloc_instrument.so ./test/malloc_test

test-clean:
	cd test && $(MAKE) clean

.PHONY: test

clean: test-clean
	-${RM} ${MALLOC_INSTRUMENT_LIB} ${OBJS} $(SRCS:.c=.d)
