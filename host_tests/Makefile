# Host test harness for the link protocol core (app/link_frame.c).
# This is the executable gate for the packet engine — run before every
# firmware commit that touches link_frame or link timing.
#
#   make          build + run
#   make ubsan    build + run with UB/address sanitizers (UB in the float
#                 encode path is a specific concern — see link_encode_bar)

CC      ?= cc
CFLAGS   = -std=c11 -Wall -Wextra -Werror -I../app

.PHONY: all run ubsan clean

all: run

test_link_frame: test_link_frame.c ../app/link_frame.c ../app/link_frame.h
	$(CC) $(CFLAGS) -O2 -o $@ test_link_frame.c ../app/link_frame.c -lm

run: test_link_frame
	./test_link_frame

test_link_frame_ubsan: test_link_frame.c ../app/link_frame.c ../app/link_frame.h
	$(CC) $(CFLAGS) -O1 -g -fsanitize=undefined,address -o $@ \
	    test_link_frame.c ../app/link_frame.c -lm

ubsan: test_link_frame_ubsan
	./test_link_frame_ubsan

clean:
	rm -f test_link_frame test_link_frame_ubsan golden_stream.csv
