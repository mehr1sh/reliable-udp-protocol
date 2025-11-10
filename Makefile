CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -Werror -O2 -MMD -MP
LDLIBS = -lcrypto

# object files
OBJS_CLIENT = client.o sham_utils.o
OBJS_SERVER = server.o sham_utils.o

# default target
all: client server

# build client
client: $(OBJS_CLIENT)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# build server
server: $(OBJS_SERVER)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# generic rule for compiling .c to .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# clean build artifacts
clean:
	rm -f client server *.o *.d *.log *.txt

# include dependency files if they exist
-include *.d
