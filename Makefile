CC=gcc
CFLAGS=-std=c99 -g -Wall -O2 -D_FILE_OFFSET_BITS=64

OBJS=dbbox.o dbfat.o cluster.o
PROG=dbbox
INCLUDES=-I/usr/include/fuse
LIBS=-pthread -lfuse -lrt -ldl

.SUFFIXES:.c .o

.c.o:
	$(CC) -c $(CFLAGS) $(DFLAGS) $(INCLUDES) $< -o $@

all:$(PROG)

$(PROG):$(OBJS)
	$(CC) $(CFLAGS) $(DFLAGS) $(OBJS) -o $@ $(LIBS)

clean:
	@rm -f *.o $(PROG) 
