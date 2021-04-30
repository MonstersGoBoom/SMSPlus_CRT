
CC = gcc

# general flags

CFLAGS = -D_MINGW -O3 -DLSB_FIRST
CFLAGS += -I smsplus\core -I crtview\source -I smsplus\shell
# files to link

OBJ = 	smsplus_crt\smsplus.o 
OBJ += smsplus\core\emu2413.o smsplus\core\error.o 	smsplus\core\fmintf.o smsplus\core\hash.o 		smsplus\core\loadrom.o 	smsplus\core\memz80.o
OBJ += smsplus\core\pio.o 		smsplus\core\render.o smsplus\core\sms.o 		smsplus\core\sn76489.o 	smsplus\core\sound.o 		smsplus\core\state.o
OBJ += smsplus\core\stream.o 	smsplus\core\system.o smsplus\core\tms.o 		smsplus\core\vdp.o 			smsplus\core\ym2413.o 	smsplus\core\z80.o

# link flags 

LDFLAGS = -s 

# OS SPECIFIC 

ifeq ($(OS),Windows_NT)
	EXT = .exe
	LDFLAGS += -l gdi32 -l winmm
endif

# work

%.m.o:	%.m
	$(CC) -c -o $@ $< $(CFLAGS)

%.o: %.c
	$(CC)  $(CFLAGS) -c -o $@ $<

sms$(EXT): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)


.PHONY: clean

clean:	
	rm $(OBJ)
