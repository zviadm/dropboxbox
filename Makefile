CC=gcc
CFLAGS=-std=c99 -g -Wall -O2 -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE 

SRCS=			\
	cluster.c	\
	dbapi.c		\
	dbbox.c		\
	dbfat.c		\
	cJSON.c

HDRS=			\
	cluster.h	\
	dbapi.h		\
	dbfat.h		\
	cJSON.h

OBJ_DIR=obj
OBJS=$(patsubst %.c, $(OBJ_DIR)/%.o, $(SRCS))

PROG=dbbox
INCLUDES=				\
	-I/usr/include/fuse \
	-I/usr/include/nss  \
	-I/usr/include/nspr

LIBS=								\
	-lm 							\
	-pthread -lfuse -lrt -ldl 		\
	-lcurl -Wl,-Bsymbolic-functions \
	-loauth

$(OBJ_DIR)/%.o: %.c $(HDRS)
	$(CC) -c $(CFLAGS) $(INCLUDES) $< -o $@

all:$(PROG)

$(PROG):$(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LIBS)

clean:
	@rm -f $(PROG) 
	@rm -f $(OBJS)
	@rm -f .depend
