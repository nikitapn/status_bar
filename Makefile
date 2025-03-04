TARGET := status_bar
CC := gcc
LIBS := -ludev -lX11 -lstdc++ -lcurl -lfmt
CXXFLAGS := -Wall -Wextra -O2

all: $(TARGET)

$(TARGET): main.cpp
	$(CC) $(CXXFLAGS) -o $@ main.cpp $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
