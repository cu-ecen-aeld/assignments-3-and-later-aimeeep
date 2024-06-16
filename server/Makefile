# the compiler: gcc for C program, define as g++ for C++
CC = $(CROSS_COMPILE)gcc

# compiler flags:
#  -g    	adds debugging information to the executable file
#  -Wall 	turns on most, but not all, compiler warnings
#  -Werror 	make the specified warning into an error
#  -O0 	disable optimizations
CFLAGS  = -Wall -Werror -g -O0

# the build target executable:
TARGET = aesdsocket

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) -o $(TARGET) $(TARGET).c

clean:
	$(RM) $(TARGET)
