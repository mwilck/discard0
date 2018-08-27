CFLAGS := -g -O2 -Wall -Wpedantic

discard0:	discard0.o
	$(CC) $(LDFLAGS) -o $@ $<

clean:
	rm -f *.o *~ discard0
