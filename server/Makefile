SRC := aesdsocket.c
TARGET ?= aesdsocket
CFLAGS ?= -g -Wall -Werror
LDFLAGS ?= -pthread
CC ?= $(CROSS_COMPILE)gcc

make: $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $(OBJS) -o $(TARGET) $(SRC) $(LDFLAGS)

all: 
	$(CC) $(CFLAGS) $(INCLUDES) $(OBJS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	-rm -f *.o $(TARGET)
