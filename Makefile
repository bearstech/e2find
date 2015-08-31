CC = gcc
CFLAGS = -O2 -Wall
LDFLAGS = -lext2fs -lcom_err -lblkid

.PHONY: all clean test

all: e2find e2find-alt

%: %.c 
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

test:
	@./test

clean:
	-rm -f e2find e2find-alt
