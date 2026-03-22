PORT = 51984
CFLAGS = -Wall -Wextra -g -DPORT=$(PORT)
TARGETS = server client

all: $(TARGETS)

server: server.c entities.h
	gcc $(CFLAGS) -o server server.c

client: client.c entities.h
	gcc $(CFLAGS) -o client client.c

clean:
	rm -f server client chat.dat
	