CXX=g++

%.o: %.c
	$(CXX) -c -o $@ $< $(CFLAGS)

all: forker.x client.x

forker.x: forker.o
	$(CXX) -o $@ $^ $(CFLAGS)

client.x: client.o
	$(CXX) -o $@ $^ $(CFLAGS)

.PHONY: clean

clean:
	rm -f *.o
