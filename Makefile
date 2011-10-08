all: mp1

CC := clang
CFLAGS := -Wextra -Wall -g -DDEBUG

SOURCEFILES = unicast.c mcast.c chat.c
HEADERS = mp1.h

mp1: $(SOURCEFILES) $(HEADERS)
	$(CC) $(CFLAGS) -pthread -o $@ $(SOURCEFILES)

clean:	restart
	-rm -f mp1 *.o

restart:
	-rm -f GROUPLIST
