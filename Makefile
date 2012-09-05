CFLAGS+=-std=c99 -Wall -pedantic -D_GNU_SOURCE -O2
LDFLAGS+=-lrt -lpthread -lz -L marcov -lmarcov

carrotd: carrotd.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

marcov/libmarcov.a:
	make -C marcov libmarcov.a

clean:
	rm -f carrotd.o carrotd

.PHONY: clean
