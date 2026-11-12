TITLE		:= Pipa Combate 3D
APPID		:= PIPA00001
CONTENTID	:= UP0001-PIPA00001_00-0000000000000000

TARGET		:= pipa_combate
BUILD		:= build
SOURCES		:= source
INCLUDES	:= include

CFLAGS		:= -O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS	:= $(CFLAGS)
LDFLAGS		:= $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS		:= -lrsx -lgcm_sys -lnet -lio -lsysutil -lsysmodule -lm

include $(PSL1GHT)/ppu_rules
