/*
 * CAN_Control.c - Triển khai driver CAN sử dụng SocketCAN trên Linux
 */

#define _GNU_SOURCE

#include "CAN_Control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>  // Thêm cho uint8_t, uint32_t, v.v. (thay xil_types.h)
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>    // Thêm thư viện này vào đầu file
#include <pthread.h> // thư viện xử lý đa luồng

LinuxCanBus Erob_Can0;
LinuxCanBus Erob_Can1;

// =============================================================================
// ĐỊNH NGHĨA CÁC BIẾN TOÀN CỤC
// =============================================================================
// Giữ lại struct này nếu logic cấp cao của bạn cần nó để làm buffer
struct CAN_Control_def CAN_Controls;

void* can0_receiver_thread(void *arg) 
{
    struct can_frame frame;
    struct pollfd fds[1];

    // Cấu hình poll để theo dõi can0
    fds[0].fd = Erob_Can0.socket_fd;
    fds[0].events = POLLIN | POLLERR; // Theo dõi cả dữ liệu và lỗi


    printf("[CAN0 Thread] Bat đau lang nghe...\n");

    while (1) {
        // Lệnh poll() này sẽ BLOCK VÔ THỜI HẠN (-1) cho đến khi có sự kiện
        // Kernel sẽ đánh thức luồng này dậy ngay khi có frame CAN
        int ret = poll(fds, 1, -1); 

        if (ret < 0) {
            perror("[CAN Thread] Loi poll(), luong ket thuc");
            break;
        }

        // Kiểm tra socket can0
        //if (fds[0].revents & POLLIN) {
        if (read(Erob_Can0.socket_fd, &frame, sizeof(frame)) > 0) {
            // XỬ LÝ FRAME TỪ CAN0 NGAY LẬP TỨC
            // (Nội dung của hàm RecvHandler cũ)
            //memcpy(CAN_Controls.RxFrame, &frame, sizeof(frame));

            // hiển thị frame nhận được từ can0 lên terminal (có thể xóa hoặc chỉnh sửa theo nhu cầu)
            printf("\033[1;34m[CAN0]\033[0m ID: 0x%X Data: ", frame.can_id);
            for (int i = 0; i < frame.can_dlc; i++) {
                printf("%02X ", frame.data[i]);
            }
            printf("\n");

        }
        //}
    }

}

void* can1_receiver_thread(void *arg) 
{
    struct can_frame frame;
    struct pollfd fds[1];

    // Cấu hình poll để theo dõi can1
    fds[0].fd = Erob_Can1.socket_fd;
    fds[0].events = POLLIN | POLLERR; // Theo dõi cả dữ liệu và lỗi

    printf("[CAN1 Thread] Bat đau lang nghe...\n");

    while (1) {
        // Lệnh poll() này sẽ BLOCK VÔ THỜI HẠN (-1) cho đến khi có sự kiện
        // Kernel sẽ đánh thức luồng này dậy ngay khi có frame CAN
        int ret = poll(fds, 1, -1); 

        if (ret < 0) {
            perror("[CAN Thread] Loi poll(), luong ket thuc");
            break;
        }

        // Kiểm tra socket can1        if (fds[0].revents & POLLIN) {
        if (read(Erob_Can1.socket_fd, &frame, sizeof(frame)) > 0) {
            // XỬ LÝ FRAME TỪ CAN1 NGAY LẬP TỨC
            // (Nội dung của hàm RecvHandler2 cũ)
            //memcpy(CAN_Controls.RxFrame2, &frame, sizeof(frame));
            // hiển thị frame nhận được từ can1 lên terminal (có thể xóa hoặc chỉnh sửa theo nhu cầu)
            printf("\033[1;35m[CAN1]\033[0m ID: 0x%X Data: ", frame.can_id);
            for (int i = 0; i < frame.can_dlc; i++) {
                printf("%02X ", frame.data[i]);
            }
            printf("\n");

        }
        //}

    }
}

// Luồng gửi CAN
void* can_tx_worker(void* arg) {
    while(1) 
    { 
        // --- Gửi dữ liệu ra CAN0 ---
        uint8_t can0_data[8] = {0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x00, 0x00};
        // Giả sử gửi giá trị ADC0 vào 2 byte đầu của CAN0
        can0_data[4] = 0;//(adc_raw[0] >> 8) & 0xFF;
        can0_data[5] = 0;//adc_raw[0] & 0xFF;
        //can_send(can0_sock, 0x123, can0_data, 8); // Gửi ID 0x123
        CAN_SendFrame(&Erob_Can0, 0x123, 8, can0_data); // Gửi ID 0x123
        usleep(100000); // 0.1 giây

        // --- Gửi dữ liệu ra CAN1 ---
        uint8_t can1_data[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
        //can_send(can1_sock, 0x456, can1_data, 8); // Gửi ID 0x456
        CAN_SendFrame(&Erob_Can1, 0x456, 8, can1_data); // Gửi ID 0x456

        usleep(100000); // 0.1 giây
    }
}

// =============================================================================
// TRIỂN KHAI CÁC HÀM API
// =============================================================================

int CAN_Init(LinuxCanBus *bus, const char* interface_name) {
    struct sockaddr_can addr;
    struct ifreq ifr;

    setup_can_hardware(interface_name, 1000000); // Cấu hình phần cứng trước khi mở socket

    // Lưu lại tên interface
    strncpy(bus->interface_name, interface_name, sizeof(bus->interface_name) - 1);
    bus->is_connected = 0;

    // 1. Tạo socket
    bus->socket_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (bus->socket_fd < 0) {
        perror("[CAN] Error creating socket");
        return -1;
    }

    // 2. Lấy chỉ số của interface (ví dụ: "can0")
    strcpy(ifr.ifr_name, bus->interface_name);
    if (ioctl(bus->socket_fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("[CAN] Error getting interface index");
        close(bus->socket_fd);
        bus->socket_fd = -1;
        return -1;
    }

    // 3. Bind socket vào interface
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(bus->socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[CAN] Error binding socket");
        close(bus->socket_fd);
        bus->socket_fd = -1;
        return -1;
    }

    bus->is_connected = 1;
    printf("[CAN] Interface %s initialized successfully.\n", bus->interface_name);
    return 0;
}

int CAN_SendFrame(LinuxCanBus *bus, u32 id, u8 dlc, const u8 *data) {
    if (!bus->is_connected || bus->socket_fd < 0) {
        return -1;
    }
    if (dlc > 8) {
        dlc = 8;
    }

    struct can_frame frame;
    frame.can_id = id;
    frame.can_dlc = dlc;
    memcpy(frame.data, data, dlc);

    if (write(bus->socket_fd, &frame, sizeof(struct can_frame)) != sizeof(struct can_frame)) {
        perror("[CAN] Error writing to CAN socket");
        return -1;
    }

    return 0;
}

int CAN_RecvFrame(LinuxCanBus *bus, u32 *id, u8 *dlc, u8 *data) {
    if (!bus->is_connected || bus->socket_fd < 0) {
        return -1;
    }

    struct can_frame frame;
    int nbytes = read(bus->socket_fd, &frame, sizeof(struct can_frame));

    if (nbytes < 0) {
        perror("[CAN] Error reading from CAN socket");
        return -1;
    }

    if (nbytes < sizeof(struct can_frame)) {
        fprintf(stderr, "[CAN] Incomplete CAN frame received\n");
        return -1;
    }

    *id = frame.can_id;
    *dlc = frame.can_dlc;
    memcpy(data, frame.data, frame.can_dlc);

    return 0;
}

void CAN_Close(LinuxCanBus *bus) {
    if (bus->socket_fd >= 0) {
        close(bus->socket_fd);
        bus->socket_fd = -1;
        bus->is_connected = 0;
        printf("[CAN] Interface %s closed.\n", bus->interface_name);
    }
}

void setup_can_hardware(const char *ifname, int bitrate) {
    char cmd[256];
    // Tắt interface trước
    sprintf(cmd, "ip link set %s down 2>/dev/null", ifname);
    system(cmd);
    
    /* Thêm "restart-ms 1000": Nếu bị Bus-off, sau 1000ms nó sẽ tự động khởi động lại.
       Tăng "txqueuelen": Để bộ đệm chứa được nhiều gói tin hơn trước khi báo lỗi.
    */
    sprintf(cmd, "ip link set %s up type can bitrate %d restart-ms 1000", ifname, bitrate);
    //sprintf(cmd, "ip link set %s up type can bitrate %d loopback on", ifname, bitrate);
    system(cmd);
    
    sprintf(cmd, "ip link set %s txqueuelen 1000", ifname);
    system(cmd);
    
    printf("[SYSTEM] %s đã được cấu hình: %d bps, tự động phục hồi sau 1000ms\n", ifname, bitrate);
}

int CAN_Init_All(void) {
    // Trước khi chạy code, bạn cần cấu hình interface từ terminal Linux:
    // > ip link set can0 up type can bitrate 1000000
    // > ip link set can1 up type can bitrate 1000000
    
    printf("[CAN] Initializing all CAN buses...\n");
    if (CAN_Init(&Erob_Can0, "can0") != 0) {
        fprintf(stderr, "[CAN] Failed to initialize can0.\n");
        return -1;
    }
    // kiem tra can1 sau khi can0 đã được mở thành công, nếu can1 lỗi thì đóng can0 để dọn dẹp
    if (CAN_Init(&Erob_Can1, "can1") != 0) {
        fprintf(stderr, "[CAN] Failed to initialize can1.\n");
        CAN_Close(&Erob_Can1); // Dọn dẹp cái đã mở trước đó
        return -1;
    }

    return 0;
}