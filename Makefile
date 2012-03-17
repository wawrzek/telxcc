CC = gcc
CCFLAGS = -O3 -Wall -std=c99
LDFLAGS = 

OBJS = telxcc.o
EXEC = telxcc

all : $(EXEC)

strip : $(EXEC)
	-strip $<

.PHONY : clean
clean :
	-rm -f $(OBJS) $(EXEC) profile.log

$(EXEC) : $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $<

%.o : %.c
	$(CC) -c $(CCFLAGS) -o $@ $<

profiled :
	make CCFLAGS="$(CCFLAGS) -fprofile-generate" LDFLAGS="$(LDFLAGS) -fprofile-generate" $(EXEC)
	find . -type f -iname \*.ts -exec sh -c './telxcc -1 -c -v -p 888 < "{}" > /dev/null 2>> profile.log' \;
	find . -type f -iname \*.ts -exec sh -c './telxcc -1 -v -p 777 < "{}" > /dev/null 2>> profile.log' \;
	find . -type f -iname \*.ts -exec sh -c './telxcc < "{}" > /dev/null 2>> profile.log' \;
	make clean
	make CCFLAGS="$(CCFLAGS) -fprofile-use" LDFLAGS="$(LDFLAGS) -fprofile-use" $(EXEC)
	-rm -f *.gcda *.gcno *.dyn pgopti.dpi pgopti.dpi.lock
