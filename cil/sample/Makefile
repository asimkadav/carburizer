CC=gcc
CFLAGS=-I.
# DEPS = hellomake.h
OBJ = %.o

CC=cilly --dodrivers
EXTRA_CFLAGS+=  --save-temps --dodrivers -I myincludes
LD=cilly --dodrivers
AR=cilly --mode=AR

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

all: %.o
#$(OBJ)

clean:
	rm -f $(OBJ)
