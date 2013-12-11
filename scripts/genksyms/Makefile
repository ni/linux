
hostprogs-y	:= genksyms
always		:= $(hostprogs-y)

genksyms-objs	:= genksyms.o parse.tab.o lex.lex.o

ifneq ($(strip $(CROSS_COMPILE)),)
TGTPRE		:= $(notdir $(CROSS_COMPILE))
tgtgenksyms	:= $(TGTPRE)genksyms

always		+= $(tgtgenksyms)

ifeq ($(KBUILD_SRC),)
KBUILD_SRC=$(CURDIR)
endif

ifeq ($(KBUILD_OUTPUT),)
KBUILD_OUTPUT=$(CURDIR)
endif

TGT_EXTRACFLAGS += -I$(KBUILD_SRC)/$(src)

$(obj)/$(TGTPRE)genksyms.o : $(src)/genksyms.c
	$(Q)$(CC) $(TGT_EXTRACFLAGS) $(CFLAGS) -c -o $@ $<

$(patsubst %,$(obj)/$(TGTPRE)%,$(filter-out %genksyms.o,$(genksyms-objs))) : $(patsubst %,$(obj)/%,$(patsubst %.o,%.c,$(genksyms-objs)))
	$(Q)$(CC) $(TGT_EXTRACFLAGS) $(CFLAGS) -c -o $@ $(KBUILD_OUTPUT)/$(subst $(TGTPRE),,$(subst .o,.c,$@))

$(obj)/$(tgtgenksyms) : $(patsubst %,$(obj)/$(TGTPRE)%,$(genksyms-objs))
	$(Q)$(CC) $(TGT_EXTRACFLAGS) $(CFLAGS) -o $@ $^
endif

# -I needed for generated C source (shipped source)
HOSTCFLAGS_parse.tab.o := -I$(src)
HOSTCFLAGS_lex.lex.o := -I$(src)

# dependencies on generated files need to be listed explicitly
$(obj)/lex.lex.o: $(obj)/keywords.hash.c $(obj)/parse.tab.h

clean-files	:= keywords.hash.c lex.lex.c parse.tab.c parse.tab.h
