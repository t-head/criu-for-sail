PLUGIN_NAME             := ppu_plugin
PLUGIN_SOBJ             := ppu_plugin.so

PLUGIN_INCLUDE          := -iquote../../include
PLUGIN_INCLUDE          += -iquote../../criu/include
PLUGIN_INCLUDE          += -iquote../../criu/arch/$(ARCH)/include/
PLUGIN_INCLUDE          += -iquote../../

__nmk_dir               ?= ../../scripts/nmk/scripts/
include $(__nmk_dir)msg.mk

CC                      ?= gcc
PLUGIN_CFLAGS           := -g -Wall -Werror -D _GNU_SOURCE -shared -nostartfiles -fPIC -DCR_PLUGIN_DEFAULT="$(PLUGINDIR)"
PLUGIN_LDFLAGS          := -lpthread -lrt
COMPEL                  := ../../compel/compel-host

all: $(PLUGIN_SOBJ)

criu-ppu.pb-c.c: criu-ppu.proto
	protoc --proto_path=. --c_out=. criu-ppu.proto

ppu_plugin.so: ppu_plugin.c ppu_plugin_util.c ppu_plugin_util.h ppu_kmif.c ppu_kmif.h ppu_ioctl.h criu-ppu.pb-c.c
	$(CC) $(PLUGIN_CFLAGS) $(shell $(COMPEL) includes) $^ -o $@ $(PLUGIN_INCLUDE) $(PLUGIN_LDFLAGS)

clean:
	$(call msg-clean, $@)
	$(Q) $(RM) ppu_plugin.so criu-ppu.pb-c.*
.PHONY: clean

mrproper: clean

install:
	$(Q) mkdir -p $(DESTDIR)$(PLUGINDIR)
	$(E) "  INSTALL " $(PLUGIN_NAME)
	$(Q) install -m 755 $(PLUGIN_SOBJ) $(DESTDIR)$(PLUGINDIR)
.PHONY: install

uninstall:
	$(E) " UNINSTALL" $(PLUGIN_NAME)
	$(Q) $(RM) $(DESTDIR)$(PLUGINDIR)/$(PLUGIN_SOBJ)
.PHONY: uninstall
