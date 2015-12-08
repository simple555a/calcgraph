CPPFLAGS += -std=c++14 -ferror-limit=5 -g

.PHONY: check format doc clean

test: test.cpp calcgraph.h
	$(CXX) $(CPPFLAGS) test.cpp $(shell pkg-config --libs --cflags cppunit boost) -o test

example: example.cpp calcgraph.h
	$(CXX) $(CPPFLAGS) example.cpp $(shell pkg-config --libs --cflags gsl boost) -o example

check: test
	time ./test

clean:
	rm -r test example *.o latex *.dSYM 2>/dev/null || true

format:
	clang-format -i *.h *.cpp

doc:
	doxygen doxygen.conf
	$(MAKE) -C latex
