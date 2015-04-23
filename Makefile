#
PREFIX   = 
CC       = $(PREFIX)gcc

# source / outputs
EXEC     = piscope

SRCS	= piscope.c
OBJS    = piscope.o

CCFLAGS = -O3 -Wall `pkg-config --cflags gtk+-3.0`
LNFLAGS = `pkg-config --libs gtk+-3.0 gmodule-2.0`

$(EXEC): $(OBJS)
	$(CC) $(OBJS) $(LNFLAGS) -o $(EXEC)

.SUFFIXES: .c .o

.c.o:
	$(CC) -c $(CCFLAGS) $<

piscope.o:	piscope.c

hf:
	cp piscope.hf     piscope

sf:
	cp piscope.sf     piscope

x86_64:
	cp piscope.x86_64 piscope

clean:
	rm -f *.o *.i *.s *~ piscope

install:
	sudo install -m 0755 -d	           /usr/local/bin
	sudo install -m 0755 -d	           /usr/share/piscope
	sudo install -m 0755 piscope       /usr/local/bin
	sudo install -m 0644 piscope.glade /usr/share/piscope

uninstall:
	sudo rm -f /usr/local/bin/piscope
	sudo rm -f /usr/share/piscope/piscope.glade
	sudo rm -f /usr/share/piscope

