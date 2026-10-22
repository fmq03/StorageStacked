CXXFLAGS += -std=c++11 -fPIC -Wall -Wextra -Wfatal-errors

CXXFLAGS += -Iinclude

LDFLAGS +=

# Debugigng
ifdef DEBUG
	CXXFLAGS += -g -O0
else
	CXXFLAGS += -O2 -DNDEBUG
endif

PROJECT = libcocogfx.a

SRCS = $(wildcard src/*.cpp)

OBJS := $(SRCS:.cpp=.o)

all: $(PROJECT)

$(PROJECT): $(OBJS)
	ar rcs $@ $^

.depend: $(SRCS)
	$(CXX) $(CXXFLAGS) -MM $^ > .depend;

clean:
	rm -rf *.a src/*.o .depend

ifneq ($(MAKECMDGOALS),clean)
    -include .depend
endif