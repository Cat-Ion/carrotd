CFLAGS+=-std=c99 -Wall -pedantic -D_GNU_SOURCE -g
LDFLAGS+=-lrt -lpthread -lz -L marcov -lmarcov

all: carrotd

carrotd: carrotd.o marcov/libmarcov.a
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

marcov/libmarcov.a:
	make -C marcov libmarcov.a

clean:
	rm -f carrotd.o carrotd

.PHONY: clean
