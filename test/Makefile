# _DEFAULT_SOURCE: glibc only declares strlcpy / strlcat outside strict -std=c11 (macOS always does).
CFLAGS ?= -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined

test: test_frame test_links
	./test_frame
	./test_links

test_frame: test_frame.c ../main/frame.c ../main/frame.h
	$(CC) $(CFLAGS) -I../main -o $@ test_frame.c ../main/frame.c

test_links: test_links.c ../main/links.c ../main/device_cache.c ../main/device_cache.h stubs/mock_idf.h
	$(CC) $(CFLAGS) -Wno-unused-parameter -Istubs -I../main -o $@ test_links.c ../main/device_cache.c

clean:
	rm -f test_frame test_links

.PHONY: test clean
