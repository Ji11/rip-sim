CC       = gcc
CFLAGS   = -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -DPOISON_REVERSE=1 -g -O2
LDFLAGS  = -lpthread -lconfig

SRCDIR   = src
OBJDIR   = obj
TARGET   = ripd

SRCS     = $(wildcard $(SRCDIR)/*.c)
OBJS     = $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))

.PHONY: all clean run1 run2 run3 test nopoison

all: $(TARGET)

# 编译时关闭毒性逆转，用于对比测试
nopoison:
	$(MAKE) clean
	$(MAKE) all CFLAGS="-Wall -Wextra -std=gnu11 -D_GNU_SOURCE -DPOISON_REVERSE=0 -g -O2"

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJDIR) $(TARGET)

# Convenience targets for running router instances
run1: $(TARGET)
	./$(TARGET) config/r1.cfg

run2: $(TARGET)
	./$(TARGET) config/r2.cfg

run3: $(TARGET)
	./$(TARGET) config/r3.cfg

# Help
help:
	@echo "RIP Router Simulator"
	@echo ""
	@echo "Build:"
	@echo "  make              Build the router binary"
	@echo "  make clean        Remove build artifacts"
	@echo ""
	@echo "Run (in separate terminals):"
	@echo "  make run1         Start router 1"
	@echo "  make run2         Start router 2"
	@echo "  make run3         Start router 3"
	@echo ""
	@echo "Management (telnet):"
	@echo "  telnet 127.0.0.1 8021   Connect to router 1"
	@echo "  telnet 127.0.0.1 8022   Connect to router 2"
	@echo "  telnet 127.0.0.1 8023   Connect to router 3"
	@echo ""
	@echo "Debug:"
	@echo "  ./ripd config/r1.cfg 2>rip.log"
