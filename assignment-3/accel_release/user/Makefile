# Variables
CC = gcc
TARGET = usertest

# Files
SRCS = usertest.c accellib.c
OBJS = $(SRCS:.c=.o)
HEADERS = interface.h

# Default target
all: $(TARGET)

# Link object files to create the executable
$(TARGET): $(OBJS)
	$(CC) -o $(TARGET) $(OBJS)

# Compile C files into object files
# Both .c files will recompile if the shared header file changes
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build files
.PHONY: all clean

clean:
	rm -f $(OBJS) $(TARGET)
