CROSS_COMPILE ?= aarch64-linux-gnu-
CC ?= $(CROSS_COMPILE)gcc

OPTEE_CLIENT_EXPORT ?= ../optee_client/out/export/usr

CFLAGS += -Wall -I$(OPTEE_CLIENT_EXPORT)/include -I../key_vault_ta
LDFLAGS += -L$(OPTEE_CLIENT_EXPORT)/lib -lteec -lpthread

SRC = vault_client.c
BIN = vault_client

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(BIN)

.PHONY: all clean
