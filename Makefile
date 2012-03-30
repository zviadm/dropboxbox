CC=gcc
CFLAGS=-std=c99 -g -Wall -O2

OBJS=dbfat.o
PROG=dbbox
INCLUDES=
LIBS= 

.SUFFIXES:.c .o

.c.o:
	$(CC) -c $(CFLAGS) $(DFLAGS) $(INCLUDES) $< -o $@

all:$(PROG)

$(PROG):$(OBJS)
	$(CC) $(CFLAGS) $(DFLAGS) $(OBJS) -o $@ $(LIBS)

clean:
	@rm -f *.o $(PROG) 
