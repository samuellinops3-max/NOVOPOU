ifeq ($(strip $(PS3DEV)),)
$(error "Please set PS3DEV in your environment.")
endif

include $(PS3DEV)/libps3dev/Makefile.define

TARGET		:= pou_ps3.elf
BUILD		:= build
SOURCE		:= .
INCLUDE		:= .
OBJS		:= main.o

CFLAGS		:= -O3 -Wall -Wextra -std=gnu99 -I$(PS3DEV)/port/include
CXXFLAGS	:= $(CFLAGS)
LDFLAGS		:= -L$(PS3DEV)/port/lib

LIBS		:= -lrsx -lio -lsysutil -llgl -lm -lnet -lpthread

all: $(BUILD)/$(TARGET) pkg

$(BUILD)/$(TARGET): $(OBJS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
	ppu-lv2objcopy -O elf32-powerpc $@ pou_ps3.self
	pkg.py --create pou_ps3.self pkg.conf

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD) $(OBJS) pou_ps3.self PARAM.SFO
