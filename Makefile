SOURCES := $(wildcard *.cpp)

CPPFLAGS += -std=c++14 -ferror-limit=5 -g

test: $(SOURCES) $(wildcard *.h)
	$(CXX) $(CPPFLAGS) $(SOURCES) $(shell pkg-config --libs --cflags cppunit boost) -o test

check: test
	./test

clean:
	rm -r test *.o latex 2>/dev/null || true

format:
	clang-format -i *.h *.cpp

doc:
	doxygen doxygen.conf
	$(MAKE) -C latex
