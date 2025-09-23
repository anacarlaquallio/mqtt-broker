CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pedantic -g
LDFLAGS =

TARGET = ep1-mqtt-broker
SOURCES = ep1-mqtt-broker.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $(TARGET)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)