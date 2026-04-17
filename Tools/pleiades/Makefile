.PHONY: build build-hook build-cli clean test

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  # macOS: build arm64 + arm64e fat binary
  ARM64E_TARGET := arm64e-apple-darwin-notlv.json
  ARM64_LIB := target/release/libpleiades_hook.dylib
  ARM64E_LIB := target/arm64e-apple-darwin-notlv/release/libpleiades_hook.dylib
  HOOK_LIB := target/libpleiades_hook.dylib
  INJECT_VAR := DYLD_INSERT_LIBRARIES
else
  # Linux: single .so
  HOOK_LIB := target/release/libpleiades_hook.so
  INJECT_VAR := LD_PRELOAD
endif

build: build-hook build-cli

ifeq ($(UNAME_S),Darwin)
build-hook:
	cargo build --release -p pleiades-hook
	cargo build --release -p pleiades-hook \
		-Z build-std -Z json-target-spec --target $(ARM64E_TARGET)
	lipo -create $(ARM64_LIB) $(ARM64E_LIB) -output $(HOOK_LIB)
	@lipo -info $(HOOK_LIB)
else
build-hook:
	cargo build --release -p pleiades-hook
	@cp target/release/libpleiades_hook.so $(HOOK_LIB) 2>/dev/null || true
	@echo "Built: $(HOOK_LIB)"
endif

build-cli:
	cargo build --release -p pleiades-cli

clean:
	cargo clean
	rm -f /tmp/pleiades-alloc.bin

test:
	cargo test --workspace

# Usage: make inject TARGET=/path/to/binary
inject: build-hook
	$(INJECT_VAR)=$(CURDIR)/$(HOOK_LIB) $(TARGET)
