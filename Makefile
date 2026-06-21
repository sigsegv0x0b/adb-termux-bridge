CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -pthread -Isrc
LDFLAGS ?= -lssl -lcrypto -pthread

SRC := src/main.c src/server.c src/tls.c src/executor.c src/handler.c src/cJSON.c
TARGET := termux-adb-bridge
TARGET_SECURE := termux-adb-bridge-secure

OBJ_NORMAL := $(SRC:src/%.c=obj/normal/%.o)
OBJ_SECURE := $(SRC:src/%.c=obj/secure/%.o)

SECURE_CERT_DIR ?= $(HOME)/.termux-adb-bridge/certs
SECURE_LDFLAGS := -l:libssl.a -l:libcrypto.a -pthread

.PHONY: all clean normal secure

all: $(TARGET_SECURE)

# Secure binary (statically linked with embedded DER certs)
$(TARGET_SECURE): $(OBJ_SECURE)
	$(CC) $(CFLAGS) -DSECURE_BUILD -o $@ $^ $(SECURE_LDFLAGS)

obj/secure/%.o: src/%.c src/certs_data.h
	@mkdir -p obj/secure
	$(CC) $(CFLAGS) -DSECURE_BUILD -c -o $@ $<

# Generate embedded cert header (also initializes certs on first build)
src/certs_data.h: | $(TARGET)
	@if ! [ -f "$(SECURE_CERT_DIR)/server.crt" ]; then \
		mkdir -p "$(SECURE_CERT_DIR)"; \
		./$(TARGET) --init-certs --cert-dir "$(SECURE_CERT_DIR)"; \
	fi
	./$(TARGET) --cert-dir "$(SECURE_CERT_DIR)" --save-certs
	./$(TARGET) --cert-dir "$(SECURE_CERT_DIR)" --embed-c-header $@

# Normal binary (needed for cert initialization and header embedding)
$(TARGET): $(OBJ_NORMAL)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

obj/normal/%.o: src/%.c
	@mkdir -p obj/normal
	$(CC) $(CFLAGS) -c -o $@ $<

normal: $(TARGET)
secure: $(TARGET_SECURE)

clean:
	rm -rf obj $(TARGET) $(TARGET_SECURE) src/certs_data.h
