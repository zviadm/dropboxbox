CC=gcc
CFLAGS=-std=c99 -g -Wall -O2 -D_FILE_OFFSET_BITS=64

SRCS=dbbox.c dbfat.c cluster.c cJSON.c
HDRS=dbfat.h cluster.h dbapi.h cJSON.h

OBJ_DIR=obj
OBJS=$(patsubst %.c, $(OBJ_DIR)/%.o, $(SRCS))

PROG=dbbox
INCLUDES=-I/usr/include/fuse
LIBS=-pthread -lfuse -lrt -ldl -lm

$(OBJ_DIR)/%.o: %.c $(HDRS)
	$(CC) -c $(CFLAGS) $(INCLUDES) $< -o $@

all:$(PROG)

$(PROG):$(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LIBS)

clean:
	@rm -f $(PROG) 
	@rm -f $(OBJS)
	@rm -f .depend
