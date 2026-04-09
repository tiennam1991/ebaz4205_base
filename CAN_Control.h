#ifndef LINUX_CAN_CONTROL_H_
#define LINUX_CAN_CONTROL_H_

#include <linux/can.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <stddef.h> // Cho size_t
#include <stdint.h>  // Thêm cho uint8_t, uint32_t, v.v. (thay xil_types.h)
#include <poll.h>    // Thêm thư viện này vào đầu file
#include <pthread.h> // thư viện xử lý đa luồng

// Định nghĩa lại các kiểu dữ liệu của Xilinx
typedef unsigned char u8_t;
typedef unsigned char u8;
typedef signed char s8_t;
typedef unsigned short u16_t;
typedef signed short s16_t;
typedef unsigned int u32_t;
typedef unsigned int u32;
typedef signed int s32_t;
typedef unsigned long long u64_t;
typedef signed long long s64_t;
typedef s8_t err_t;

// typedef unsigned char uint8_t;
// typedef unsigned short uint16_t;
// typedef signed short int16_t;
// typedef unsigned int uint32_t;
// =============================================================================
// ĐỊNH NGHĨA VÀ STRUCT
// =============================================================================

/*
 * Các hằng số cho CAN, có thể giữ lại nếu logic ứng dụng cần
 */
#define TEST_MESSAGE_ID		0x643
#define FRAME_DATA_LENGTH	8 // Có thể thay đổi

struct Erob_Struct
{
	uint32_t 			UDP_counter;
	uint32_t 			Stop_counter;
	uint32_t 			Erob_Freq;
	uint32_t 			Erob_Freq2;
	uint32_t 			Erob_Gpio_data;
	uint32_t 			Erob_SW1;
	uint32_t 			Erob_SW2;
	uint32_t 			Erob_SW3;
	uint32_t 			Erob_SW4;
	uint32_t 			Erob_Counter;
	int32_t 			Erob_Speed;
	int32_t 			Erob_Position;
	int32_t 			Erob_Current;
	int32_t 			Erob_Voltage;
	int32_t 			Erob_Temperature;
	uint32_t 			Erob_Encoder;
	int					Zynq_Encoder;
	char 				str[80];
	uint8_t				UDP_Data[100];
};

// --- Struct quản lý bus CAN trên Linux ---
typedef struct {
    int socket_fd;          // File descriptor cho CAN socket
    char interface_name[16]; // Tên giao diện (ví dụ: "can0")
    int is_connected;       // Cờ trạng thái
} LinuxCanBus;

struct CAN_Control_def
	{
	uint8_t CAN_CRC_bit[100];
	uint16_t Erob_CRC2;

	uint16_t CAN_Start_of_Frame;// = 0;
	uint16_t CAN_ID;// = 643;
	uint16_t CAN_DLC;// = 8;
	uint16_t CAN_Requ_Remote;// = 0;
	uint16_t CAN_ID_EXt_Bit;// = 0;
	uint16_t CAN_Reserved;// = 0;
	uint16_t CAN_Data[8];
	uint16_t CAN_CRC_Delimiter;//= 1;
	uint16_t CAN_Acknow_Slot_Bit;// = 1;
	uint16_t CAN_ACKnow_Delimiter;// = 0;
	uint16_t CAN_CRC;// = 0x0000;
	uint16_t CAN_EOF;// = 0x7f;
	uint16_t CAN_IFS;// = 0x07;
	uint16_t CAN_Time;// = 100;
	uint32_t CAN_Freq;// = 100000;
	uint16_t CAN_Start;// = 1;

	uint8_t CAN_Bits_Val[100];
	uint32_t CAN_Bits1;// = 0x00000000;
	uint32_t CAN_Bits2;// = 0x00000000;
	uint32_t CAN_Bits3;// = 0x00000000;
	uint32_t CAN_Bits4;// = 0x00000000;
	uint32_t CAN_CMD;//   = 0x00000000;
	uint32_t CAN_DIR2;//  = 1;

	u32 TxFrame[20];//XCANPS_MAX_FRAME_SIZE_IN_WORDS];
	u32 RxFrame[20];//XCANPS_MAX_FRAME_SIZE_IN_WORDS];

	u32 TxFrame2[20];//XCANPS_MAX_FRAME_SIZE_IN_WORDS];
	u32 RxFrame2[20];//XCANPS_MAX_FRAME_SIZE_IN_WORDS];

	uint16_t Erob_CRC;
	uint8_t Erob_Cmd[30];

	uint32_t			FifoFull1_Timeout; // sua loi bai toan tinh
	uint32_t			Motor1_Timeout; // sua loi bai toan tinh
	uint8_t				Motor1_disconect; // sua loi bai toan tinh
	uint8_t				Motor1_Restart; // sua loi bai toan tinh
	uint8_t				Motor1_OK; // sua loi bai toan tinh
	uint32_t			Motor1_Restart_Time; // sua loi bai toan tinh

	uint32_t			FifoFull2_Timeout; // sua loi bai toan tinh
	uint32_t			Motor2_Timeout; // sua loi bai toan tinh
	uint8_t				Motor2_disconect; // sua loi bai toan tinh
	uint8_t				Motor2_Restart; // sua loi bai toan tinh
	uint8_t				Motor2_OK; // sua loi bai toan tinh
	uint32_t			Motor2_Restart_Time; // sua loi bai toan tinh

	};

// =============================================================================
// KHAI BÁO CÁC BIẾN TOÀN CỤC
// =============================================================================

// Thay thế Erob_Can0 và Erob_Can1 bằng các struct quản lý của Linux
extern LinuxCanBus Erob_Can0;
extern LinuxCanBus Erob_Can1;

// =============================================================================
// KHAI BÁO CÁC HÀM API
// =============================================================================

/**
 * @brief Khởi tạo và mở một bus CAN.
 * @param bus - Con trỏ tới struct LinuxCanBus cần khởi tạo.
 * @param interface_name - Tên của giao diện CAN (ví dụ: "can0").
 * @return 0 nếu thành công, -1 nếu thất bại.
 */
int CAN_Init(LinuxCanBus *bus, const char* interface_name);

/**
 * @brief Gửi một frame CAN.
 * @param bus - Con trỏ tới bus CAN đã được khởi tạo.
 * @param id - CAN ID của frame.
 * @param dlc - Độ dài dữ liệu (0-8).
 * @param data - Con trỏ tới buffer chứa dữ liệu.
 * @return 0 nếu thành công, -1 nếu thất bại.
 */
int CAN_SendFrame(LinuxCanBus *bus, u32 id, u8 dlc, const u8 *data);

/**
 * @brief Đọc một frame CAN (hàm này sẽ block cho đến khi có dữ liệu).
 * @param bus - Con trỏ tới bus CAN đã được khởi tạo.
 * @param id - Con trỏ để lưu CAN ID của frame nhận được.
 * @param dlc - Con trỏ để lưu độ dài dữ liệu của frame nhận được.
 * @param data - Buffer để lưu dữ liệu nhận được (phải đủ lớn, ít nhất 8 bytes).
 * @return 0 nếu thành công, -1 nếu thất bại.
 */
int CAN_RecvFrame(LinuxCanBus *bus, u32 *id, u8 *dlc, u8 *data);

/**
 * @brief Đóng và giải phóng tài nguyên của một bus CAN.
 * @param bus - Con trỏ tới bus CAN cần đóng.
 */
void CAN_Close(LinuxCanBus *bus);

/**
 * @brief Hàm khởi tạo cấp cao, gọi các hàm init cho cả hai bus.
 *        Hàm này thay thế cho CAN_Innit_Data() cũ.
 * @return 0 nếu thành công, -1 nếu thất bại.
 */
int CAN_Init_All(void);

void* can0_receiver_thread(void *arg);
void* can1_receiver_thread(void *arg);
void* can_tx_worker(void* arg);
void setup_can_hardware(const char *ifname, int bitrate);

#endif /* LINUX_CAN_CONTROL_H_ */