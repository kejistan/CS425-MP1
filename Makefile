all: mp1

SOURCEFILES = unicast.c mcast.c chat.c
HEADERS = mp1.h
CC = gcc

mp1: $(SOURCEFILES) $(HEADERS)
	$(CC) -g -pthread -o $@ $(SOURCEFILES)

clean:	restart
	-rm -f mp1 *.o

restart:
	-rm -f GROUPLIST
