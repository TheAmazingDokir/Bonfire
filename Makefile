TARGETS = server client

all: $(TARGETS)

%: %.c
	gcc -Wall -g -o $@ $^