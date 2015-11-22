SOURCES := $(wildcard *.cpp)
# Objs are all the sources, with .cpp replaced by .o
OBJS := $(SOURCES:.cpp=.o)

# link
test: $(OBJS)
	$(CXX) $(OBJS) $(shell pkg-config --libs cppunit) -o test

check: test
	./test
	
# pull in dependency info for *existing* .o files
-include $(OBJS:.o=.d)

CPPFLAGS += -std=c++14 -ferror-limit=3 -g
CPPFLAGS += $(shell pkg-config --cflags cppunit)

# compile and generate dependency info;
# more complicated dependency computation, so all prereqs listed
# will also become command-less, prereq-less targets
#   sed:    strip the target (everything before colon)
#   sed:    remove any continuation backslashes
#   fmt -1: list words one per line
#   sed:    strip leading spaces
#   sed:    add trailing colons
%.o: %.c
	$(CXX) -c $(CPPFLAGS) $*.cpp -o $*.o
	$(CXX) -MM $(CPPFLAGS) $*.cpp > $*.d
	@mv -f $*.d $*.d.tmp
	@sed -e 's|.*:|$*.o:|' < $*.d.tmp > $*.d
	@sed -e 's/.*://' -e 's/\\$$//' < $*.d.tmp | fmt -1 | sed -e 's/^ *//' -e 's/$$/:/' >> $*.d
	@rm -f $*.d.tmp

# remove compilation products
clean:
	rm -f test *.o *.d
