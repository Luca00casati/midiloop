CC=gcc
CFLAG=-Wall -Wextra -g 
BIN=usb_midi
.PHONY: all setup

all: main.c
	$(CC) main.c $(CFLAG) -lusb-1.0 -o $(BIN)

setup: setup-usb-midi.sh
	./setup-usb-midi.sh
