LIBS = -lext2fs -lcom_err -lblkid
CC = gcc
CFLAGS = -g -Wall

.PHONY: default all clean

all: e2find e2find2

%.o: %.c 
	$(CC) $(CFLAGS) -c $< -o $@

e2find: e2find.o
	$(CC) $(<) -Wall $(LIBS) -o $@

e2find2: e2find2.o
	$(CC) $(<) -Wall $(LIBS) -o $@

clean:
	-rm -f *.o e2find e2find2
