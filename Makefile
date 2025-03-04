TARGET := status_bar
CC := gcc
LIB := -ludev -lX11 -lstdc++ -lcurl -lfmt
CXXFLAGS := -Wall -Wextra -O2

all: $(TARGET)

$(TARGET): main.cpp
	$(CC) $(CXXFLAGS) -o $@ main.cpp $(LIB)

clean:
	rm $(TARGET)
