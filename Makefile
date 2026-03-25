PORT = 51984
CFLAGS = -Wall -Wextra -g -DPORT=$(PORT)
TARGETS = server client

all: $(TARGETS)

server: server.c channels.c entities.h channels.h constants.h set_ops.h
	gcc $(CFLAGS) -o server server.c channels.c

client: client.c entities.h
	gcc $(CFLAGS) -o client client.c

clean:
	rm -f server client chat.dat
	