CC = gcc
CFLAGS = -Wall -Wextra -O2
LIBS = -lcurl

SRCS = main.c attendiq.c cJSON.c
OBJS = $(SRCS:.c=.o)
TARGET = attendiq

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
