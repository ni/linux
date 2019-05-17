# SPDX-License-Identifier: GPL-2.0
OBJECT_FILES_NON_STANDARD := y

hostprogs-y	:= modpost mk_elfconfig
always		:= $(hostprogs-y) empty.o

modpost-objs	:= modpost.o file2alias.o sumversion.o

devicetable-offsets-file := devicetable-offsets.h

$(obj)/$(devicetable-offsets-file): $(obj)/devicetable-offsets.s FORCE
	$(call filechk,offsets,__DEVICETABLE_OFFSETS_H__)

targets += $(devicetable-offsets-file) devicetable-offsets.s

# dependencies on generated files need to be listed explicitly

$(obj)/modpost.o $(obj)/file2alias.o $(obj)/sumversion.o: $(obj)/elfconfig.h
$(obj)/file2alias.o: $(obj)/$(devicetable-offsets-file)

quiet_cmd_elfconfig = MKELF   $@
      cmd_elfconfig = $(obj)/mk_elfconfig < $< > $@

ifneq ($(CROSS_COMPILE),)
TGTPRE := $(notdir $(CROSS_COMPILE))
tgtmodpost	:= $(TGTPRE)modpost

always += $(tgtmodpost)

ifeq ($(KBUILD_SRC),)
KBUILD_SRC=$(CURDIR)
endif

ifeq ($(KBUILD_OUTPUT),)
KBUILD_OUTPUT=$(CURDIR)
endif

$(patsubst %,$(obj)/$(TGTPRE)%,$(modpost-objs)): $(KBUILD_SRC)/$(src)/$(TGTPRE)elfconfig.h | $(obj)/modpost
	$(Q)$(CC) $(TGT_EXTRACFLAGS) $(CFLAGS) -I$(KBUILD_OUTPUT)/$(obj) -DCONFIG_MODPOST_ELFCONFIG="\"$(TGTPRE)elfconfig.h\"" -c -o $@ $(KBUILD_SRC)/$(subst $(TGTPRE),,$(subst .o,.c,$@))

$(obj)/$(tgtmodpost) : $(patsubst %,$(obj)/$(TGTPRE)%,$(modpost-objs))
	$(Q)$(CC) $(TGT_EXTRACFLAGS) $(CFLAGS) -o $@ $^
endif

$(obj)/elfconfig.h: $(obj)/empty.o $(obj)/mk_elfconfig FORCE
	$(call if_changed,elfconfig)

targets += elfconfig.h
