CXX=g++ -O3

%.o: %.c
	$(CXX) -c -o $@ $< $(CFLAGS)

all: forker.x client.x

forker.x: forker.o
	$(CXX) -o $@ $^ $(CFLAGS) -pthread

client.x: client.o
	$(CXX) -o $@ $^ $(CFLAGS)

.PHONY: clean

clean:
	rm -f *.o
