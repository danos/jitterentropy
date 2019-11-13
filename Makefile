# Compile Noise Source as user space application

CC=gcc
override CFLAGS +=-Wall -pie -fPIE -Wl,-z,relro,-z,now

NAME := jitterentropy-rngd
#C_SRCS := $(wildcard *.c)
C_SRCS := jitterentropy-base.c jitterentropy-rngd.c
C_OBJS := ${C_SRCS:.c=.o}
OBJS := $(C_OBJS)

INCLUDE_DIRS :=
LIBRARY_DIRS :=
LIBRARIES := rt

CFLAGS += $(foreach includedir,$(INCLUDE_DIRS),-I$(includedir))
LDFLAGS += $(foreach librarydir,$(LIBRARY_DIRS),-L$(librarydir))
LDFLAGS += $(foreach library,$(LIBRARIES),-l$(library))

.PHONY: all clean distclean

all: $(NAME)

$(NAME): $(OBJS)
#	scan-build --use-analyzer=/usr/bin/clang $(CC) $(OBJS) -o $(NAME) $(LDFLAGS)
	$(CC) $(OBJS) -o $(NAME) $(LDFLAGS)

clean:
	@- $(RM) $(NAME)
	@- $(RM) $(OBJS)

distclean: clean
