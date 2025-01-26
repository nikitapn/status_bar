TARGET := status_bar
CC := gcc
LIB := -ludev -lX11
CFLAGS := -Wall -Wextra -O2

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ main.c $(LIB)

clean:
	rm $(TARGET)
