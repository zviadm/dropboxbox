CC=gcc
CFLAGS=-std=c99 -g -Wall -O2 -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE 

SRCS=			\
	cluster.c	\
	dbapi.c		\
	dbbox.c		\
	dbfat.c		\
	dbfiles.c	\
	cJSON.c

HDRS=			\
	cluster.h	\
	dbapi.h		\
	dbfat.h		\
	dbfiles.h	\
	cJSON.h

OBJ_DIR=obj
OBJS=$(patsubst %.c, $(OBJ_DIR)/%.o, $(SRCS))

PROG=dbbox
INCLUDES=				\
	-I/usr/include/fuse \
	-I/usr/include/nspr

LIBS=								\
	-lm -liconv -loauth				\
	-pthread -lfuse -lrt -ldl		\
	-lcurl -Wl,-Bsymbolic-functions

all:$(PROG)

$(OBJ_DIR):
	mkdir $(OBJ_DIR)

$(OBJ_DIR)/%.o: %.c $(HDRS)
	$(CC) -c $(CFLAGS) $(INCLUDES) $< -o $@

$(PROG):$(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LIBS)

clean:
	@rm -f $(PROG) 
	@rm -f $(OBJS)
	@rm -f .depend
