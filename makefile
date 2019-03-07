IISHELL = /bin/sh
CC = g++

CFLAGS = -std=c++11 -g -m64 -Wall -I"/usr/include/GL" -I"/usr/include" -I"src/" -I"../.." -I"/opt/local/include" -O3 -DNDEBUG -ffast-math -Wno-deprecated-declarations 
LDFLAGS = -g -m64 -L"/opt/local/lib" -lglut -lGL  -lGL -lGLU -lglut

TARGET  = tinsel

SOURCES = $(wildcard src/*.cpp src/cjson/*.c)
HEADERS = $(wildcard src/*.h)
TESTS = $(wildcard src/tests/*.h)

OBJECTS = $(SOURCES:.cpp=.o) 

all: $(TARGET)
	./$(TARGET) data/transmission.tin 

$(TARGET): $(OBJECTS) makefile $(TESTS)
	$(CC)  $(OBJECTS) $(LDFLAGS) -o $(TARGET) 

clean:
	-rm -f $(OBJECTS)
	-rm -f $(TARGET)

%.o: %.cpp $(HEADERS) $(TESTS) makefile
	$(CC) $(CFLAGS) -c -o $@ $<


run: $(TARGET)
	./$(TARGET)

.PHONY : all clean
