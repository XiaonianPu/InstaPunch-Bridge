CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined

test: test_frame
	./test_frame

test_frame: test_frame.c ../main/frame.c ../main/frame.h
	$(CC) $(CFLAGS) -I../main -o $@ test_frame.c ../main/frame.c

clean:
	rm -f test_frame

.PHONY: test clean
