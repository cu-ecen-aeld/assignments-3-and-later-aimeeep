CC:=$(CROSS_COMPILE)gcc
exe:=writer
obj:=writer.o

all:$(obj)
	$(CC) -o $(exe) $(obj)  
%.o:%.c
	$(CC) -c $^ -o $@

.PHONY:clean
clean:
	rm -rf $(obj) $(exe)

