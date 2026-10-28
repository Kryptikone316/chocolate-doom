# WAD loader for the SF3500. Same toolchain as the game:
#   make TC=/path/to/buildroot/output/host
TC ?= $(HOME)/handhelds/sf3000-work/buildroot/output/host
CC := $(TC)/bin/mips-mti-linux-gnu-gcc
SF := ../src/sf3500

wadloader: wadloader.c bitfont.c $(SF)/hwdisp.c $(SF)/joykey.c
	$(CC) -O2 -Wall -I$(SF) -I. -o $@ $^ -ldl -lpthread -lm

clean:
	rm -f wadloader
