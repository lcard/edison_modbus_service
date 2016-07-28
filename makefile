P=modbus_service
CFLAGS=-g -Wall -std=gnu99 -O3 `pkg-config --cflags mraa --cflags libmodbus`
LDLIBS=`pkg-config --libs mraa --libs libmodbus`
objects=modbus_service.o 
$(P): $(objects)

clean:
	rm -rf *.o $(P)
