TARGET = usbshell
OBJS = main.o USBHostFS.o exports.o

# Use the kernel's small inbuilt libc
USE_KERNEL_LIBC = 1
# Use only kernel libraries
USE_KERNEL_LIBS = 1

INCDIR = 
#CFLAGS = -O2 -G0 -Wall -fno-builtin-printf -DDEBUG
CFLAGS = -Os -G0 -Wall -fno-builtin-printf -I../usbhostfs
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)
LDFLAGS = -L../libpsplink

LIBDIR =

LIBS = -lpsplink

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak
