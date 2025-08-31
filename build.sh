#!/bin/sh
set -xe
gcc main.c -Wall -Wextra -lusb-1.0 -g -o usb_midi
