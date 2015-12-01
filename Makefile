SOURCES := $(wildcard *.cpp)

CPPFLAGS += -std=c++14 -ferror-limit=5 -g

test: $(SOURCES) $(wildcard *.h)
	$(CXX) $(CPPFLAGS) $(SOURCES) $(shell pkg-config --libs --cflags cppunit boost) -o test

check: test
	./test

clean:
	rm -f test *.o
