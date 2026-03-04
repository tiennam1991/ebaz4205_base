# Tên của file thực thi cuối cùng
TARGET = main_app

# --- Phần cấu hình Toolchain ---
# Các biến CC, CFLAGS, LDFLAGS... sẽ được lấy tự động từ môi trường SDK sau khi bạn 'source'
# Chúng ta sử dụng ?= để chỉ gán nếu biến đó chưa tồn tại
CC      ?= arm-xilinx-linux-gnueabi-gcc
CXX     ?= arm-xilinx-linux-gnueabi-g++
AS      ?= arm-xilinx-linux-gnueabi-as
LD      ?= arm-xilinx-linux-gnueabi-ld

# --- Phần cấu hình đường dẫn Header ---
# SDKTARGETSYSROOT là biến môi trường do SDK cung cấp trỏ đến thư mục sysroot
EXTRA_INC = -I. \
            -I./rfdc \
            -I$(SDKTARGETSYSROOT)/usr/include

# Cờ biên dịch bổ sung (giữ lại các cờ mặc định của SDK và thêm cờ riêng)
MY_CFLAGS = -Wall -O2 -g $(EXTRA_INC)

# Cờ liên kết (Linker) bổ sung
# Thêm -lmetal, -lgpiod nếu bạn đã cài các thư viện này vào rootfs
MY_LDFLAGS = -lm -lpthread

# --- Tự động tìm source code ---
SRCS := $(shell find . -name '*.c')
OBJS := $(SRCS:.c=.o)
DEPS := $(OBJS:.o=.d)

# --- Các quy tắc (Rules) ---

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "Linking target: $@"
	$(CC) $(LDFLAGS) -o $@ $^ $(MY_LDFLAGS)

%.o: %.c
	@echo "Compiling: $<"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(MY_CFLAGS) -c $< -o $@ -MMD -MP -MF"$(@:%.o=%.d)"

clean:
	@echo "Cleaning project..."
	rm -f $(TARGET) $(OBJS) $(DEPS)

-include $(DEPS)

.PHONY: all clean