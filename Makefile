CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -O2 -g
CXXFILES = err.cpp client.cpp my_rand.cpp crc32.cpp
OFILES = $(CXXFILES:.cpp=.o)


.PHONY:clean

COMPILE.cpp = $(CXX) $(CXXFLAGS) -c

all: client

client: $(OFILES)
	$(CXX) $(CXXFLAGS) -o screen-worms-client $(OFILES) -lstdc++fs

%.o : %.cpp %.h
	$(COMPILE.cpp) $< -lstdc++fs -o $@


clean:
	-rm -f $(OFILES) screen-worms-client
