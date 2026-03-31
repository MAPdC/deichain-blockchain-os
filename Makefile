CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -std=c11 -g
LDFLAGS = -lpthread -lrt -lcrypto

SRCDIR  = src
BINDIR  = .

CONTROLLER = $(BINDIR)/controller
MINER      = $(BINDIR)/miner
VALIDATOR  = $(BINDIR)/validator
STATISTICS = $(BINDIR)/statistics
TXGEN      = $(BINDIR)/txgen

SHARED_OBJ = $(SRCDIR)/shared.o $(SRCDIR)/logging.o

all: $(CONTROLLER) $(MINER) $(VALIDATOR) $(STATISTICS) $(TXGEN)

$(CONTROLLER): $(SRCDIR)/controller.o $(SHARED_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(MINER): $(SRCDIR)/miner.o $(SHARED_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(VALIDATOR): $(SRCDIR)/validator.o $(SHARED_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(STATISTICS): $(SRCDIR)/statistics.o $(SHARED_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TXGEN): $(SRCDIR)/txgen.o $(SHARED_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

debug: CFLAGS += -DDEBUG
debug: clean all

clean:
	rm -f $(SRCDIR)/*.o
	rm -f $(CONTROLLER) $(MINER) $(VALIDATOR) $(STATISTICS) $(TXGEN)
	rm -f DEIChain_log.log VALIDATOR_PIPE

run: all
	./$(CONTROLLER)

.PHONY: all clean debug run
