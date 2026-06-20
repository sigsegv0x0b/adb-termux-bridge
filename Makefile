CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -pthread -Isrc
LDFLAGS ?= -lssl -lcrypto -pthread

SRC := src/main.c src/server.c src/tls.c src/executor.c src/handler.c src/json.c
TARGET := termux-adb-bridge

OBJ_NORMAL := $(SRC:src/%.c=obj/normal/%.o)

.PHONY: all clean secure

all: $(TARGET)

$(TARGET): $(OBJ_NORMAL)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

obj/normal/%.o: src/%.c
	@mkdir -p obj/normal
	$(CC) $(CFLAGS) -c -o $@ $<

# --- Secure build (embedded certs) ---
# Bootstrap: make && ./termux-adb-bridge --init-certs && make secure

SECURE_CERT_DIR ?= $(HOME)/.termux-adb-bridge/certs
TARGET_SECURE := termux-adb-bridge-secure
OBJ_SECURE := $(SRC:src/%.c=obj/secure/%.o)
SECURE_LDFLAGS := -l:libssl.a -l:libcrypto.a -pthread

secure: $(TARGET)
	@if ! [ -f src/certs_data.h ]; then \
		echo "Saving certs to fingerprint directory..."; \
		./$(TARGET) --cert-dir "$(SECURE_CERT_DIR)" --save-certs; \
		echo "Generating embedded header..."; \
		./$(TARGET) --cert-dir "$(SECURE_CERT_DIR)" --embed-c-header src/certs_data.h; \
	fi
	$(MAKE) $(TARGET_SECURE)

$(TARGET_SECURE): $(OBJ_SECURE)
	$(CC) $(CFLAGS) -DSECURE_BUILD -o $@ $^ $(SECURE_LDFLAGS)

obj/secure/%.o: src/%.c src/certs_data.h
	@mkdir -p obj/secure
	$(CC) $(CFLAGS) -DSECURE_BUILD -c -o $@ $<

clean:
	rm -rf obj $(TARGET) $(TARGET_SECURE) src/certs_data.h
