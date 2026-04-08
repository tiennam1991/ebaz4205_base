#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>

int can0_sock, can1_sock;

// --- Cấu hình ---
#define UART_COUNT      10
#define MAP_SIZE        0x10000
#define WEB_PORT        80
#define GPIO_BASE       906 
#define REMOTE_IP       "192.168.1.120"
#define UDP_TX_PORT     2027
#define UDP_RX_PORT     2026
#define SPI_PATH        "/dev/spidev0.0"

// --- Cấu trúc ---
typedef struct { int fd; volatile unsigned int *ptr; } uio_dev_t;
typedef struct { 
    int id; int fd; char path[20]; 
    char last_rx[64]; char last_tx[64]; 
    pthread_mutex_t lock;
} uart_info_t;

// --- Biến toàn cục ---
uio_dev_t pwm, enc;
uart_info_t u_info[UART_COUNT];
float cpu_val = 0;
int adc_raw[8];      // Dữ liệu thô từ MCP3208
int gpio_in[4], gpio_out[4];
int out_pins[] = {56, 57, 54, 55}; 
int in_pins[]  = {58, 59, 60, 61}; 

// --- Hàm bổ trợ ---
void clean_str(char *s) { for (; *s; s++) if (!isprint(*s) || *s == '"' || *s == '\\') *s = ' '; }

void gpio_op(int pin, char *file, char *val) {
    char p[64]; sprintf(p, "/sys/class/gpio/gpio%d/%s", pin + GPIO_BASE, file);
    int fd = open(p, O_WRONLY); 
    if(fd >= 0) { write(fd, val, strlen(val)); close(fd); }
}

// --- 1. Luồng ADC MCP3208 ---
void* adc_worker(void* arg) {
    int fd = open(SPI_PATH, O_RDWR);
    if (fd < 0) return NULL;
    uint8_t mode = SPI_MODE_0, bits = 8; uint32_t speed = 1000000;
    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    while(1) {
        for(int i=0; i<8; i++) {
            uint8_t tx[3] = { 0x06 | ((i & 0x04) >> 2), (i & 0x03) << 6, 0x00 };
            uint8_t rx[3] = {0};
            struct spi_ioc_transfer tr = { .tx_buf=(unsigned long)tx, .rx_buf=(unsigned long)rx, .len=3, .speed_hz=speed };
            if(ioctl(fd, SPI_IOC_MESSAGE(1), &tr) > 0) 
                adc_raw[i] = ((rx[1] & 0x0F) << 8) | rx[2];
        }
        usleep(100000); // Cập nhật 10Hz
    }
    close(fd); return NULL;
}

// --- 2. Luồng tính % CPU ---
void update_cpu() {
    static long long last_u, last_n, last_s, last_i;
    long long u, n, s, i;
    FILE *fp = fopen("/proc/stat", "r");
    if (fp) {
        fscanf(fp, "cpu %lld %lld %lld %lld", &u, &n, &s, &i); fclose(fp);
        long long d_w = (u+n+s) - (last_u+last_n+last_s);
        long long d_t = (u+n+s+i) - (last_u+last_n+last_s+last_i);
        if(d_t > 0) cpu_val = (float)d_w / d_t * 100.0;
        last_u=u; last_n=n; last_s=s; last_i=i;
    }
}

// Luồng Heartbeat (LED0)
void* led_heartbeat_worker(void* arg) {
    int s = 0; while(1) { s=!s; gpio_op(out_pins[2], "value", s?"1":"0"); gpio_out[2]=s; usleep(500000); }
}

// Luồng UART nhận data
void* uart_worker(void* arg) {
    uart_info_t *ui = (uart_info_t*)arg;
    ui->fd = open(ui->path, O_RDWR | O_NOCTTY | O_NDELAY);
    if(ui->fd < 0) return NULL;
    struct termios cfg; tcgetattr(ui->fd, &cfg);
    cfsetispeed(&cfg, B115200); cfsetospeed(&cfg, B115200);
    cfg.c_cflag |= (CLOCAL | CREAD | CS8); tcsetattr(ui->fd, TCSANOW, &cfg);
    while(1) {
        char buf[64]; int n = read(ui->fd, buf, 63);
        if(n > 0) { buf[n]=0; clean_str(buf); pthread_mutex_lock(&ui->lock); strcpy(ui->last_rx, buf); pthread_mutex_unlock(&ui->lock); }
        usleep(100000);
    }
}

// --- 3. Luồng Web Server + Dashboard ---
void* web_worker(void* arg) {
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = { .sin_family=AF_INET, .sin_port=htons(WEB_PORT), .sin_addr.s_addr=INADDR_ANY };
    bind(sfd, (struct sockaddr *)&addr, sizeof(addr)); listen(sfd, 10);

    while(1) {
        int cfd = accept(sfd, NULL, NULL); if(cfd < 0) continue;
        gpio_op(out_pins[3], "value", "1"); gpio_out[3] = 1;
        char req[1024]; int rlen = read(cfd, req, 1023);
        if (rlen > 0) {
            req[rlen] = 0;
            if (strstr(req, "GET /set")) {
                int p, v; sscanf(strstr(req, "p="), "p=%d&v=%d", &p, &v);
                if(p>=0 && p<2) { char vs[2]; sprintf(vs,"%d",v); gpio_op(out_pins[p], "value", vs); gpio_out[p]=v; }
            }
            if (strstr(req, "GET /data")) {
                update_cpu(); char json[8192];
                int p = sprintf(json, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n"
                    "{\"cpu\":%.1f,\"pwm\":[%u,%u,%u,%u],\"adc\":[%d,%d,%d,%d,%d,%d,%d,%d],\"gpio_in\":[%d,%d,%d,%d],\"gpio_out\":[%d,%d,%d,%d],\"uarts\":[",
                    cpu_val, pwm.ptr[0], pwm.ptr[1], pwm.ptr[2], pwm.ptr[3],
                    adc_raw[0],adc_raw[1],adc_raw[2],adc_raw[3],adc_raw[4],adc_raw[5],adc_raw[6],adc_raw[7],
                    gpio_in[0], gpio_in[1], gpio_in[2], gpio_in[3], gpio_out[0], gpio_out[1], gpio_out[2], gpio_out[3]);
                for(int i=0; i<UART_COUNT; i++) p += sprintf(json+p, "{\"rx\":\"%s\"}%s", u_info[i].last_rx, (i==9?"":","));
                sprintf(json+p, "]}"); write(cfd, json, strlen(json));
            } else {
                const char *html = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>EBAZ4205 Industrial</title>"
                    "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>"
                    "<style>body{background:#111;color:#eee;font-family:sans-serif;display:grid;grid-template-columns:1fr 1fr;gap:15px;padding:15px}"
                    ".card{background:#222;padding:15px;border-radius:12px;border:1px solid #444} canvas{max-height:160px} "
                    "table{width:100%;border-collapse:collapse;font-size:12px} th,td{border:1px solid #333;padding:5px;text-align:center}"
                    ".led{width:12px;height:12px;border-radius:50%;display:inline-block;background:#333} .on{background:#0f0;box-shadow:0 0 8px #0f0}"
                    ".btn{background:#444;color:white;border:none;padding:5px 10px;border-radius:4px;cursor:pointer} .active{background:#05f}</style></head><body>"
                    "<div class='card'><h3>CPU: <span id='cpv'>0</span>%</h3><canvas id='c0'></canvas></div>"
                    "<div class='card'><h3>MCP3208 ADC (12-bit)</h3><canvas id='c1'></canvas></div>"
                    "<div class='card' style='grid-column:span 2'><h3>Real-time Sensors Data</h3><table id='atb'></table></div>"
                    "<div class='card'><h3>Control</h3>"
                    "<button id='b0' onclick='set(0,1)'>TRIG 1 ON</button><button onclick='set(0,0)'>OFF</button><br><br>"
                    "<button id='b1' onclick='set(1,1)'>TRIG 2 ON</button><button onclick='set(1,0)'>OFF</button></div>"
                    "<div class='card'><h3>UARTs</h3><table id='utb'></table></div>"
                    "<script>"
                    "const fC=(id,ls,cs)=>new Chart(id,{type:'line',data:{labels:[],datasets:ls.map((l,i)=>({label:l,data:[],borderColor:cs[i],pointRadius:0}))},options:{animation:false}});"
                    "let charts; window.onload=()=>{ "
                    "  charts=[fC('c0',['CPU'],['#0f0']), fC('c1',['CH0','CH1','CH2'],['#f00','#0f0','#0af'])]; upd(); setInterval(upd,1000); };"
                    "function set(p,v){ fetch(`/set?p=${p}&v=${v}`); }"
                    "function upd(){ fetch('/data').then(r=>r.json()).then(d=>{"
                    "  document.getElementById('cpv').innerText=d.cpu; const now=new Date().toLocaleTimeString();"
                    "  [[d.cpu], [d.adc[0],d.adc[1],d.adc[2]]].forEach((v,i)=>{ charts[i].data.labels.push(now); v.forEach((val,j)=>charts[i].data.datasets[j].data.push(val));"
                    "  if(charts[i].data.labels.length>20){charts[i].data.labels.shift(); charts[i].data.datasets.forEach(ds=>ds.data.shift());} charts[i].update(); });"
                    "  document.getElementById('atb').innerHTML='<tr>'+d.adc.map((v,i)=>`<th>CH${i}</th>`).join('')+'</tr><tr>'+d.adc.map(v=>`<td>${v}<br>${(v*3.3/4095).toFixed(2)}V</td>`).join('')+'</tr>';"
                    "  document.getElementById('utb').innerHTML=d.uarts.slice(0,5).map((u,i)=>`<tr><td>U${i+1}</td><td>${u.rx}</td></tr>`).join('');"
                    "}); }</script></body></html>";
                write(cfd, html, strlen(html));
            }
        }
        close(cfd); usleep(50000); gpio_op(out_pins[3], "value", "0"); gpio_out[3] = 0;
    }
}

// Luồng UDP
void* udp_tx_worker(void* arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in rem = { .sin_family=AF_INET, .sin_port=htons(UDP_TX_PORT) };
    inet_pton(AF_INET, REMOTE_IP, &rem.sin_addr);
    while(1) 
    { 
        char b[128]; 
        sprintf(b, "ADC0:%d CPU:%.1f", adc_raw[0], cpu_val); 
        sendto(sock, b, strlen(b), 0, (struct sockaddr*)&rem, sizeof(rem)); 
        sleep(1); 
    }
}

// Hàm gửi dữ liệu ra một cổng UART cụ thể (index 0-9)
void uart_send(int index, const char *msg) {
    if (index < 0 || index >= UART_COUNT || u_info[index].fd < 0) return;

    // Gửi dữ liệu xuống phần cứng
    int len = strlen(msg);
    write(u_info[index].fd, msg, len);

    // Cập nhật vào bộ nhớ đệm để hiển thị lên Web Dashboard
    pthread_mutex_lock(&u_info[index].lock);
    strncpy(u_info[index].last_tx, msg, 63);
    u_info[index].last_tx[63] = '\0'; // Đảm bảo kết thúc chuỗi
    pthread_mutex_unlock(&u_info[index].lock);
}

int init_can(const char *ifname) {
    int s;
    struct sockaddr_can addr;
    struct ifreq ifr;

    // 1. Tạo socket
    if ((s = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
        perror("SocketCAN: Tạo socket thất bại");
        return -1;
    }

    // 2. Xác định index của giao diện (can0, can1)
    strcpy(ifr.ifr_name, ifname);
    ioctl(s, SIOCGIFINDEX, &ifr);

    // 3. Bind socket vào giao diện
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("SocketCAN: Bind thất bại");
        return -1;
    }
    return s;
}

int can_send(int sock, uint32_t id, uint8_t *data, uint8_t len) {
    struct can_frame frame;
    
    frame.can_id = id;          // ID của gói tin CAN
    frame.can_dlc = len > 8 ? 8 : len; // CAN tiêu chuẩn tối đa 8 byte
    memcpy(frame.data, data, frame.can_dlc);

    if (write(sock, &frame, sizeof(struct can_frame)) != sizeof(struct can_frame)) {
        perror("SocketCAN: Gửi thất bại");
        return -1;
    }
    return 0;
}

int main() {
    pthread_t t[20];
    int f0 = open("/dev/uio0", O_RDWR); pwm.ptr = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, f0, 0);
    int f1 = open("/dev/uio1", O_RDWR); enc.ptr = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, f1, 0);
    
    for(int i=0; i<4; i++) { 
        char cmd[128]; sprintf(cmd, "echo %d > /sys/class/gpio/export 2>/dev/null", out_pins[i]+GPIO_BASE); system(cmd);
        sprintf(cmd, "echo out > /sys/class/gpio/gpio%d/direction", out_pins[i]+GPIO_BASE); system(cmd);
        sprintf(cmd, "echo %d > /sys/class/gpio/export 2>/dev/null", in_pins[i]+GPIO_BASE); system(cmd);
        sprintf(cmd, "echo in > /sys/class/gpio/gpio%d/direction", in_pins[i]+GPIO_BASE); system(cmd);
    }

    pthread_create(&t[0], NULL, led_heartbeat_worker, NULL);
    pthread_create(&t[1], NULL, web_worker, NULL);
    pthread_create(&t[2], NULL, udp_tx_worker, NULL);
    pthread_create(&t[3], NULL, adc_worker, NULL); // Chạy luồng đọc ADC

    for(int i=0; i<UART_COUNT; i++) {
        u_info[i].id=i+1; sprintf(u_info[i].path, "/dev/ttyUL%d", i+1);
        pthread_mutex_init(&u_info[i].lock, NULL);
        pthread_create(&t[i+4], NULL, uart_worker, &u_info[i]);
    }

    can0_sock = init_can("can0");
    can1_sock = init_can("can1");

    if(can0_sock >= 0) printf("CAN0 đã sẵn sàng!\n");
    if(can1_sock >= 0) printf("CAN1 đã sẵn sàng!\n");

    int send_time = 0;
    printf("Ebaz4205 v8 Full Integrated Ready! Web on Port 80, ADC MCP3208 Active.\n");
    while(1) 
    {
        char path[64], val;
        for(int i=0; i<4; i++) {
            sprintf(path, "/sys/class/gpio/gpio%d/value", in_pins[i] + GPIO_BASE);
            int fd = open(path, O_RDONLY); if(fd>=0) { read(fd, &val, 1); gpio_in[i]=(val=='1'); close(fd); }
        }
        usleep(200000);

        send_time++;
        if(send_time >= 5) 
        { 
            // Mỗi 5 chu kỳ (1 giây), gửi dữ liệu ADC0 ra UART1
            send_time = 0;
            char msg[64]; // Chỉ cần 1 buffer dùng chung cho vòng lặp

            // --- Gửi dữ liệu ra CAN0 ---
            uint8_t can0_data[8] = {0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x00, 0x00};
            // Giả sử gửi giá trị ADC0 vào 2 byte đầu của CAN0
            can0_data[4] = (adc_raw[0] >> 8) & 0xFF;
            can0_data[5] = adc_raw[0] & 0xFF;
            can_send(can0_sock, 0x123, can0_data, 8); // Gửi ID 0x123

            // --- Gửi dữ liệu ra CAN1 ---
            uint8_t can1_data[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
            can_send(can1_sock, 0x456, can1_data, 8); // Gửi ID 0x456

            for (int i = 0; i < UART_COUNT; i++) 
            {
                // Tạo nội dung thông báo
                sprintf(msg, "UART %d ready (ADC0: %d)\r\n", i + 1, adc_raw[0]);
                
                // Gửi dữ liệu
                uart_send(i, msg);
            }
        }
    }
    return 0;
}

// #define _GNU_SOURCE
// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <unistd.h>
// #include <fcntl.h>
// #include <pthread.h>
// #include <termios.h>
// #include <sys/mman.h>
// #include <sys/socket.h>
// #include <netinet/in.h>
// #include <arpa/inet.h>
// #include <ctype.h>

// // --- Cấu hình ---
// #define UART_COUNT      10
// #define MAP_SIZE        0x10000
// #define WEB_PORT        80
// #define GPIO_BASE       906 
// #define REMOTE_IP       "192.168.1.120"
// #define UDP_TX_PORT     2027
// #define UDP_RX_PORT     2026

// #define NUM_PORTS 10

// // Mảng chứa đường dẫn các thiết bị
// const char *uart_paths[NUM_PORTS] = {
//     "/dev/ttyUL1", "/dev/ttyUL2", "/dev/ttyUL3", "/dev/ttyUL4", "/dev/ttyUL5",
//     "/dev/ttyUL6", "/dev/ttyUL7", "/dev/ttyUL8", "/dev/ttyUL9", "/dev/ttyUL10"
// };

// // --- Cấu trúc ---
// typedef struct { int fd; volatile unsigned int *ptr; } uio_dev_t;
// typedef struct { 
//     int id; int fd; char path[20]; 
//     char last_rx[64]; char last_tx[64]; 
//     pthread_mutex_t lock;
// } uart_info_t;

// // --- Biến toàn cục ---
// uio_dev_t pwm, enc;
// uart_info_t u_info[UART_COUNT];
// float cpu_val = 0;
// int gpio_in[4], gpio_out[4];
// int out_pins[] = {56, 57, 54, 55}; // 0:Trig1, 1:Trig2, 2:Led0, 3:Led1
// int in_pins[]  = {58, 59, 60, 61}; // Input0-3

// // --- Hàm bổ trợ: Làm sạch chuỗi cho JSON ---
// void clean_str(char *s) {
//     for (; *s; s++) if (!isprint(*s) || *s == '"' || *s == '\\') *s = ' ';
// }

// // --- Hàm thao tác GPIO Sysfs ---
// void gpio_op(int pin, char *file, char *val) {
//     char p[64]; sprintf(p, "/sys/class/gpio/gpio%d/%s", pin + GPIO_BASE, file);
//     int fd = open(p, O_WRONLY); 
//     if(fd >= 0) { 
//         write(fd, val, strlen(val)); 
//         close(fd); 
//     }
// }

// // --- 1. Luồng tính % CPU ---
// void update_cpu() {
//     static long long last_u, last_n, last_s, last_i;
//     long long u, n, s, i;
//     FILE *fp = fopen("/proc/stat", "r");
//     if (fp) {
//         fscanf(fp, "cpu %lld %lld %lld %lld", &u, &n, &s, &i);
//         fclose(fp);
//         long long diff_work = (u+n+s) - (last_u+last_n+last_s);
//         long long diff_total = (u+n+s+i) - (last_u+last_n+last_s+last_i);
//         if(diff_total > 0) cpu_val = (float)diff_work / diff_total * 100.0;
//         last_u=u; last_n=n; last_s=s; last_i=i;
//     }
// }

// // --- 2. Luồng LED0: Nhấp nháy Heartbeat 0.5s ---
// void* led_heartbeat_worker(void* arg) {
//     int state = 0;
//     while(1) {
//         state = !state;
//         gpio_op(out_pins[2], "value", state ? "1" : "0");
//         gpio_out[2] = state; // Cập nhật trạng thái để hiển thị Web
//         usleep(500000); // 0.5 giây
//     }
// }

// // --- 3. Luồng UART ---
// void* uart_worker(void* arg) {
//     uart_info_t *ui = (uart_info_t*)arg;
//     ui->fd = open(ui->path, O_RDWR | O_NOCTTY | O_NDELAY);
//     if(ui->fd < 0) return NULL;
//     struct termios cfg; tcgetattr(ui->fd, &cfg);
//     cfsetispeed(&cfg, B115200); cfsetospeed(&cfg, B115200);
//     cfg.c_cflag |= (CLOCAL | CREAD | CS8); tcsetattr(ui->fd, TCSANOW, &cfg);
//     while(1) {
//         char buf[64]; int n = read(ui->fd, buf, 63);
//         if(n > 0) { 
//             buf[n] = 0; clean_str(buf);
//             pthread_mutex_lock(&ui->lock); strcpy(ui->last_rx, buf); pthread_mutex_unlock(&ui->lock);
//         }
//         usleep(100000);
//     }
// }

// // --- 4. Luồng Web Server + LED1 Activity ---
// void* web_worker(void* arg) {
//     int sfd = socket(AF_INET, SOCK_STREAM, 0);
//     int opt = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
//     struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(WEB_PORT), .sin_addr.s_addr = INADDR_ANY };
//     bind(sfd, (struct sockaddr *)&addr, sizeof(addr)); listen(sfd, 10);

//     while(1) {
//         int cfd = accept(sfd, NULL, NULL);
//         if(cfd < 0) continue;

//         // BẬT LED 1 khi có truy cập
//         gpio_op(out_pins[3], "value", "1");
//         gpio_out[3] = 1;

//         char req[1024]; int rlen = read(cfd, req, 1023);
//         if (rlen > 0) {
//             req[rlen] = 0;
//             if (strstr(req, "GET /set")) { // Điều khiển GPIO từ Web
//                 int p, v; sscanf(strstr(req, "p="), "p=%d&v=%d", &p, &v);
//                 if(p>=0 && p<2) { // Chỉ cho phép điều khiển Trigger 1, 2
//                     char vs[2]; sprintf(vs,"%d",v); 
//                     gpio_op(out_pins[p], "value", vs); gpio_out[p]=v; 
//                 }
//             }
//             if (strstr(req, "GET /data")) { // Gửi dữ liệu JSON
//                 update_cpu();
//                 char json[8192];
//                 int p = sprintf(json, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n"
//                                       "{\"cpu\":%.1f,\"pwm\":[%u,%u,%u,%u],\"enc\":[%u,%u],\"gpio_in\":[%d,%d,%d,%d],\"gpio_out\":[%d,%d,%d,%d],\"uarts\":[",
//                                       cpu_val, pwm.ptr[0], pwm.ptr[1], pwm.ptr[2], pwm.ptr[3], enc.ptr[0], enc.ptr[1],
//                                       gpio_in[0], gpio_in[1], gpio_in[2], gpio_in[3],
//                                       gpio_out[0], gpio_out[1], gpio_out[2], gpio_out[3]);
//                 for(int i=0; i<UART_COUNT; i++) {
//                     p += sprintf(json+p, "{\"rx\":\"%s\",\"tx\":\"%s\"}%s", u_info[i].last_rx, u_info[i].last_tx, (i==9?"":","));
//                 }
//                 sprintf(json+p, "]}"); write(cfd, json, strlen(json));
//             } else { // Gửi giao diện HTML
//                 const char *html = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
//                         "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Ebaz4205 Dashboard</title>"
//                         "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>"
//                         "<style>"
//                         "body{background:#111;color:#eee;font-family:sans-serif;display:grid;grid-template-columns:1fr 1fr;gap:15px;padding:15px}"
//                         ".card{background:#222;padding:15px;border-radius:12px;border:1px solid #444;box-shadow:0 4px 10px rgba(0,0,0,0.5)}"
//                         "canvas{max-height:180px} table{width:100%;border-collapse:collapse;font-size:13px;margin-top:10px}"
//                         "th,td{border:1px solid #333;padding:8px} th{background:#333;color:#aaa}"
//                         ".led{width:14px;height:14px;border-radius:50%;display:inline-block;vertical-align:middle;margin-right:8px;border:1px solid #444}"
//                         ".led-red.on{background:#f00;box-shadow:0 0 10px #f00}"
//                         ".led-green.on{background:#0f0;box-shadow:0 0 10px #0f0}"
//                         ".led-blue.on{background:#0af;box-shadow:0 0 10px #0af}"
//                         ".btn-group{margin-bottom:10px;display:flex;align-items:center;justify-content:space-between;background:#2d2d2d;padding:8px;border-radius:8px}"
//                         "button{background:#444;color:white;border:none;padding:6px 12px;border-radius:4px;cursor:pointer;margin-left:5px}"
//                         "button:hover{background:#666} .active{background:#05f!important}"
//                         "</style></head><body>"
                        
//                         "<div class='card'><h3><span class='led led-green on'></span>CPU: <span id='cpv'>0</span>%</h3><canvas id='c0'></canvas></div>"
//                         "<div class='card'><h3>PWM 4-Channels Output</h3><canvas id='c1'></canvas></div>"
//                         "<div class='card'><h3>Encoder 2-Channels Input</h3><canvas id='c2'></canvas></div>"

//                         "<div class='card'><h3>GPIO Control</h3>"
//                         // --- PHỤC HỒI GIAO DIỆN CŨ CHO TRIGGERS ---
//                         "  <div class='btn-group'><span>Trigger 1 (Pin 54)</span><div><button id='b0_1' onclick='set(0,1)'>ON</button><button id='b0_0' onclick='set(0,0)'>OFF</button></div></div>"
//                         "  <div class='btn-group'><span>Trigger 2 (Pin 55)</span><div><button id='b1_1' onclick='set(1,1)'>ON</button><button id='b1_0' onclick='set(1,0)'>OFF</button></div></div>"
//                         // --- HIỂN THỊ TRẠNG THÁI LED ---
//                         "  <div class='btn-group'><span>LED 0 (Heartbeat)</span><span id='l0' class='led led-blue'></span></div>"
//                         "  <div class='btn-group'><span>LED 1 (Web Activity)</span><span id='l1' class='led led-blue'></span></div>"
//                         "</div>"

//                         "<div class='card' style='grid-column: span 2'><h3>Industrial Inputs (EMIO)</h3>"
//                         "  <div style='display:flex; justify-content:space-around; background:#2d2d2d; padding:15px; border-radius:10px'>"
//                         "    <div><span id='in0' class='led led-red'></span>IN 0</div><div><span id='in1' class='led led-red'></span>IN 1</div>"
//                         "    <div><span id='in2' class='led led-red'></span>IN 2</div><div><span id='in3' class='led led-red'></span>IN 3</div>"
//                         "  </div>"
//                         "</div>"

//                         "<div class='card' style='grid-column: span 2'><h3>UART Monitor</h3>"
//                         "  <table><thead><tr><th>Port</th><th>Last RX</th><th>Last TX</th></tr></thead><tbody id='utb'></tbody></table>"
//                         "</div>"

//                         "<script>"
//                         "const fC=(id,ls,cs)=>new Chart(id,{type:'line',data:{labels:[],datasets:ls.map((l,i)=>({label:l,data:[],borderColor:cs[i],tension:0.2,pointRadius:0}))},options:{animation:false,scales:{y:{grid:{color:'#333'}}}}});"
//                         "let charts; window.onload=()=>{ "
//                         "  charts=[fC('c0',['CPU'],['#0f0']),fC('c1',['CH1','CH2','CH3','CH4'],['#f00','#0f0','#0af','#ff0']),fC('c2',['ENC_A','ENC_B'],['#f0f','#fff'])]; "
//                         "  setInterval(upd,1000); "
//                         "};"
//                         "function set(p,v){ fetch(`/set?p=${p}&v=${v}`); }"
//                         "function upd(){ fetch('/data').then(r=>r.json()).then(d=>{"
//                         "  document.getElementById('cpv').innerText=d.cpu; const now=new Date().toLocaleTimeString();"
//                         "  [ [d.cpu],d.pwm,d.enc ].forEach((v,i)=>{ charts[i].data.labels.push(now); v.forEach((val,j)=>charts[i].data.datasets[j].data.push(val));"
//                         "  if(charts[i].data.labels.length>20){charts[i].data.labels.shift(); charts[i].data.datasets.forEach(ds=>ds.data.shift());} charts[i].update(); });"
//                         /* Cập nhật đèn LED và trạng thái nút bấm */
//                         "  for(let i=0;i<4;i++) document.getElementById('in'+i).className = d.gpio_in[i]?'led led-red on':'led led-red';"
//                         "  document.getElementById('l0').className=d.gpio_out[2]?'led led-blue on':'led led-blue';"
//                         "  document.getElementById('l1').className=d.gpio_out[3]?'led led-blue on':'led led-blue';"
//                         "  document.getElementById('b0_1').className = d.gpio_out[0]?'active':''; document.getElementById('b0_0').className = !d.gpio_out[0]?'active':'';"
//                         "  document.getElementById('b1_1').className = d.gpio_out[1]?'active':''; document.getElementById('b1_0').className = !d.gpio_out[1]?'active':'';"
//                         "  document.getElementById('utb').innerHTML=d.uarts.map((u,i)=>`<tr><td><b>UART ${i+1}</b></td><td style='color:#0f0'>${u.rx}</td><td style='color:#0af'>${u.tx}</td></tr>`).join('');"
//                         "}); }"
//                         "</script></body></html>";
//                 write(cfd, html, strlen(html));
//             }
//         }
//         close(cfd);

//         // TẮT LED 1 sau khi xử lý xong (tạo hiệu ứng nhấp nháy cực nhanh)
//         usleep(50000); // Giữ đèn sáng 50ms để mắt kịp thấy
//         gpio_op(out_pins[3], "value", "0");
//         gpio_out[3] = 0;
//     }
// }

// // --- 5. Luồng UDP (Batching TX) ---
// void* udp_tx_worker(void* arg) {
//     int sock = socket(AF_INET, SOCK_DGRAM, 0);
//     struct sockaddr_in rem = { .sin_family=AF_INET, .sin_port=htons(UDP_TX_PORT) };
//     inet_pton(AF_INET, REMOTE_IP, &rem.sin_addr);
//     struct mmsghdr msgs[4]; struct iovec iovs[4]; char b[4][128];
//     memset(msgs, 0, sizeof(msgs));
//     for(int i=0; i<4; i++){ iovs[i].iov_base=b[i]; iovs[i].iov_len=128; msgs[i].msg_hdr.msg_iov=&iovs[i]; msgs[i].msg_hdr.msg_iovlen=1; msgs[i].msg_hdr.msg_name=&rem; msgs[i].msg_hdr.msg_namelen=sizeof(rem); }
//     while(1) {
//         for(int i=0; i<4; i++) sprintf(b[i], "STATUS: PWM=%u ENC=%u", pwm.ptr[0], enc.ptr[0]);
//         sendmmsg(sock, msgs, 4, 0); usleep(1000000);
//     }
// }

// int main() {
//     pthread_t t[16];
    
//     // Khởi tạo UIO
//     int f0 = open("/dev/uio0", O_RDWR); pwm.ptr = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, f0, 0);
//     int f1 = open("/dev/uio1", O_RDWR); enc.ptr = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, f1, 0);
    
//     // Xuất bản và cấu hình hướng cho GPIO (EMIO 54-57 và 69-72)
//     char cmd[128];
//     for(int i=0; i<4; i++) { 
//         sprintf(cmd, "echo %d > /sys/class/gpio/export 2>/dev/null", out_pins[i]+GPIO_BASE); system(cmd);
//         sprintf(cmd, "echo out > /sys/class/gpio/gpio%d/direction", out_pins[i]+GPIO_BASE); system(cmd);
//         sprintf(cmd, "echo %d > /sys/class/gpio/export 2>/dev/null", in_pins[i]+GPIO_BASE); system(cmd);
//         sprintf(cmd, "echo in > /sys/class/gpio/gpio%d/direction", in_pins[i]+GPIO_BASE); system(cmd);
//     }

//     // Khởi tạo luồng
//     pthread_create(&t[0], NULL, led_heartbeat_worker, NULL); // LED0 nhấp nháy
//     pthread_create(&t[1], NULL, web_worker, NULL);           // LED1 nhấp nháy ở đây
//     pthread_create(&t[2], NULL, udp_tx_worker, NULL);

//     for(int i=0; i<UART_COUNT; i++) {
//         u_info[i].id=i+1; sprintf(u_info[i].path, "/dev/ttyUL%d", i+1);
//         strcpy(u_info[i].last_rx, "-"); strcpy(u_info[i].last_tx, "-");
//         pthread_mutex_init(&u_info[i].lock, NULL);
//         pthread_create(&t[i+3], NULL, uart_worker, &u_info[i]);
//     }

//     printf("Hệ thống Ebaz4205 v7 Sẵn sàng! LED0: Heartbeat, LED1: Web Activity\n");
    
//     while(1) { // Đọc Input định kỳ
//         char path[64], val;
//         for(int i=0; i<4; i++) {
//             sprintf(path, "/sys/class/gpio/gpio%d/value", in_pins[i] + GPIO_BASE);
//             int fd = open(path, O_RDONLY); if(fd>=0) { read(fd, &val, 1); gpio_in[i] = (val=='1'); close(fd); }
//         }
//         usleep(200000);
//     }
//     return 0;
// }

