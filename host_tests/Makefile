# Host test harness for the link protocol core (app/link_frame.c).
# This is the executable gate for the packet engine — run before every
# firmware commit that touches link_frame or link timing.
#
#   make          build + run
#   make ubsan    build + run with UB/address sanitizers (UB in the float
#                 encode path is a specific concern — see link_encode_bar)

CC      ?= cc
CFLAGS   = -std=c11 -Wall -Wextra -Werror -I../app -DAPP_ENABLE_SIM=1

.PHONY: all run ubsan refs checkrefs clean

all: run

test_link_frame: test_link_frame.c ../app/link_frame.c ../app/link_frame.h
	$(CC) $(CFLAGS) -O2 -o $@ test_link_frame.c ../app/link_frame.c -lm

run: test_link_frame checkrefs
	./test_link_frame

# The committed reference streams in refs/ are what the bench PC verifies
# captures against (it has Keil and Python, but no C toolchain). Regenerate
# them here and fail on any difference, so the code and the shipped references
# can never drift apart unnoticed.
REFSPECS = phaseB_rate1000:1000:2 phaseA_rate1000:1000:1 \
           full_rate1000:1000:0    phaseB_rate100:100:2

checkrefs: test_link_frame
	@tmp=`mktemp -d`; fail=0; \
	for spec in $(REFSPECS); do \
	  name=`echo $$spec | cut -d: -f1`; rate=`echo $$spec | cut -d: -f2`; \
	  ph=`echo $$spec | cut -d: -f3`; \
	  ./test_link_frame --emit-ref $$tmp/$$name.csv $$rate $$ph >/dev/null; \
	  if ! cmp -s $$tmp/$$name.csv refs/$$name.csv; then \
	    echo "STALE REFERENCE: refs/$$name.csv no longer matches the profile"; \
	    fail=1; \
	  fi; \
	done; \
	rm -rf $$tmp; \
	if [ $$fail -ne 0 ]; then \
	  echo "  -> run 'make refs' if the profile change was deliberate"; \
	  exit 1; \
	fi; \
	echo "refs/ up to date with the profile"

refs: test_link_frame
	@for spec in $(REFSPECS); do \
	  name=`echo $$spec | cut -d: -f1`; rate=`echo $$spec | cut -d: -f2`; \
	  ph=`echo $$spec | cut -d: -f3`; \
	  ./test_link_frame --emit-ref refs/$$name.csv $$rate $$ph; \
	done

test_link_frame_ubsan: test_link_frame.c ../app/link_frame.c ../app/link_frame.h
	$(CC) $(CFLAGS) -O1 -g -fsanitize=undefined,address -o $@ \
	    test_link_frame.c ../app/link_frame.c -lm

ubsan: test_link_frame_ubsan
	./test_link_frame_ubsan

clean:
	rm -f test_link_frame test_link_frame_ubsan golden_stream.csv
