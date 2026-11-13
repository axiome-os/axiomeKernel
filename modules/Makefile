REPO_ROOT := $(realpath ../..)

# Tool locations are overridable via environment or command line, e.g.:
#   make CROSS_PREFIX=/opt/axiome/cross/bin
CROSS_PREFIX ?= $(HOME)/opt/cross/bin
# Only fall back to the local cross toolchain when CC/LD are not supplied by
# the parent build (exported env) or the command line.
ifeq ($(filter command line environment override,$(origin CC)),)
CC := $(CROSS_PREFIX)/x86_64-elf-gcc
endif
ifeq ($(filter command line environment override,$(origin LD)),)
LD := $(CROSS_PREFIX)/x86_64-elf-ld
endif

# Modules are freestanding, position-independent in practice via the large
# code model: every cross-module reference is a 64-bit absolute relocation,
# resolved by the kernel loader against the .ksymtab export table.
CFLAGS := -ffreestanding -nostdlib -nostartfiles -mno-red-zone -mcmodel=large \
          -mno-mmx -mno-sse -mno-sse2 -O2 -g -pipe -Wall -Wextra \
          -I$(REPO_ROOT) -I$(REPO_ROOT)/kernel

# e1000 + hello stay loadable. sata/nvme sources live here too but link
# into kernel.elf as built-ins (see kernel/Makefile + AXIOME_BUILTIN_DRIVER).
MODULES := e1000.kxt hello.kxt

.PHONY: all clean
all: $(MODULES)

# Compile to a relocatable object, then `ld -r` to keep relocations and the
# symbol table so the kernel loader can relocate + resolve at load time.
%.kxt: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $*.o $<
	$(LD) -r -o $@ $*.o
	rm -f $*.o

clean:
	rm -f $(MODULES) *.o
