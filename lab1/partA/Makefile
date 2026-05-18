CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c99
TARGET  = ioperf

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ main.c

clean:
	rm -f $(TARGET) testfile writefile results.csv

.PHONY: all clean
