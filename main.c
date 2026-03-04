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

// --- Cấu hình ---
#define UART_COUNT      10
#define MAP_SIZE        0x10000
#define WEB_PORT        80
#define GPIO_BASE       906 
#define REMOTE_IP       "192.168.1.123"
#define UDP_TX_PORT     2027
#define UDP_RX_PORT     2026

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
int gpio_in[4], gpio_out[4];
int out_pins[] = {56, 57, 54, 55}; // 0:Trig1, 1:Trig2, 2:Led0, 3:Led1
int in_pins[]  = {58, 59, 60, 61}; // Input0-3

// --- Hàm bổ trợ: Làm sạch chuỗi cho JSON ---
void clean_str(char *s) {
    for (; *s; s++) if (!isprint(*s) || *s == '"' || *s == '\\') *s = ' ';
}

// --- Hàm thao tác GPIO Sysfs ---
void gpio_op(int pin, char *file, char *val) {
    char p[64]; sprintf(p, "/sys/class/gpio/gpio%d/%s", pin + GPIO_BASE, file);
    int fd = open(p, O_WRONLY); 
    if(fd >= 0) { 
        write(fd, val, strlen(val)); 
        close(fd); 
    }
}

// --- 1. Luồng tính % CPU ---
void update_cpu() {
    static long long last_u, last_n, last_s, last_i;
    long long u, n, s, i;
    FILE *fp = fopen("/proc/stat", "r");
    if (fp) {
        fscanf(fp, "cpu %lld %lld %lld %lld", &u, &n, &s, &i);
        fclose(fp);
        long long diff_work = (u+n+s) - (last_u+last_n+last_s);
        long long diff_total = (u+n+s+i) - (last_u+last_n+last_s+last_i);
        if(diff_total > 0) cpu_val = (float)diff_work / diff_total * 100.0;
        last_u=u; last_n=n; last_s=s; last_i=i;
    }
}

// --- 2. Luồng LED0: Nhấp nháy Heartbeat 0.5s ---
void* led_heartbeat_worker(void* arg) {
    int state = 0;
    while(1) {
        state = !state;
        gpio_op(out_pins[2], "value", state ? "1" : "0");
        gpio_out[2] = state; // Cập nhật trạng thái để hiển thị Web
        usleep(500000); // 0.5 giây
    }
}

// --- 3. Luồng UART ---
void* uart_worker(void* arg) {
    uart_info_t *ui = (uart_info_t*)arg;
    ui->fd = open(ui->path, O_RDWR | O_NOCTTY | O_NDELAY);
    if(ui->fd < 0) return NULL;
    struct termios cfg; tcgetattr(ui->fd, &cfg);
    cfsetispeed(&cfg, B115200); cfsetospeed(&cfg, B115200);
    cfg.c_cflag |= (CLOCAL | CREAD | CS8); tcsetattr(ui->fd, TCSANOW, &cfg);
    while(1) {
        char buf[64]; int n = read(ui->fd, buf, 63);
        if(n > 0) { 
            buf[n] = 0; clean_str(buf);
            pthread_mutex_lock(&ui->lock); strcpy(ui->last_rx, buf); pthread_mutex_unlock(&ui->lock);
        }
        usleep(100000);
    }
}

// --- 4. Luồng Web Server + LED1 Activity ---
void* web_worker(void* arg) {
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(WEB_PORT), .sin_addr.s_addr = INADDR_ANY };
    bind(sfd, (struct sockaddr *)&addr, sizeof(addr)); listen(sfd, 10);

    while(1) {
        int cfd = accept(sfd, NULL, NULL);
        if(cfd < 0) continue;

        // BẬT LED 1 khi có truy cập
        gpio_op(out_pins[3], "value", "1");
        gpio_out[3] = 1;

        char req[1024]; int rlen = read(cfd, req, 1023);
        if (rlen > 0) {
            req[rlen] = 0;
            if (strstr(req, "GET /set")) { // Điều khiển GPIO từ Web
                int p, v; sscanf(strstr(req, "p="), "p=%d&v=%d", &p, &v);
                if(p>=0 && p<2) { // Chỉ cho phép điều khiển Trigger 1, 2
                    char vs[2]; sprintf(vs,"%d",v); 
                    gpio_op(out_pins[p], "value", vs); gpio_out[p]=v; 
                }
            }
            if (strstr(req, "GET /data")) { // Gửi dữ liệu JSON
                update_cpu();
                char json[8192];
                int p = sprintf(json, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n"
                                      "{\"cpu\":%.1f,\"pwm\":[%u,%u,%u,%u],\"enc\":[%u,%u],\"gpio_in\":[%d,%d,%d,%d],\"gpio_out\":[%d,%d,%d,%d],\"uarts\":[",
                                      cpu_val, pwm.ptr[0], pwm.ptr[1], pwm.ptr[2], pwm.ptr[3], enc.ptr[0], enc.ptr[1],
                                      gpio_in[0], gpio_in[1], gpio_in[2], gpio_in[3],
                                      gpio_out[0], gpio_out[1], gpio_out[2], gpio_out[3]);
                for(int i=0; i<UART_COUNT; i++) {
                    p += sprintf(json+p, "{\"rx\":\"%s\",\"tx\":\"%s\"}%s", u_info[i].last_rx, u_info[i].last_tx, (i==9?"":","));
                }
                sprintf(json+p, "]}"); write(cfd, json, strlen(json));
            } else { // Gửi giao diện HTML
                const char *html = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                        "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Ebaz4205 Dashboard</title>"
                        "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>"
                        "<style>"
                        "body{background:#111;color:#eee;font-family:sans-serif;display:grid;grid-template-columns:1fr 1fr;gap:15px;padding:15px}"
                        ".card{background:#222;padding:15px;border-radius:12px;border:1px solid #444;box-shadow:0 4px 10px rgba(0,0,0,0.5)}"
                        "canvas{max-height:180px} table{width:100%;border-collapse:collapse;font-size:13px;margin-top:10px}"
                        "th,td{border:1px solid #333;padding:8px} th{background:#333;color:#aaa}"
                        ".led{width:14px;height:14px;border-radius:50%;display:inline-block;vertical-align:middle;margin-right:8px;border:1px solid #444}"
                        ".led-red.on{background:#f00;box-shadow:0 0 10px #f00}"
                        ".led-green.on{background:#0f0;box-shadow:0 0 10px #0f0}"
                        ".led-blue.on{background:#0af;box-shadow:0 0 10px #0af}"
                        ".btn-group{margin-bottom:10px;display:flex;align-items:center;justify-content:space-between;background:#2d2d2d;padding:8px;border-radius:8px}"
                        "button{background:#444;color:white;border:none;padding:6px 12px;border-radius:4px;cursor:pointer;margin-left:5px}"
                        "button:hover{background:#666} .active{background:#05f!important}"
                        "</style></head><body>"
                        
                        "<div class='card'><h3><span class='led led-green on'></span>CPU: <span id='cpv'>0</span>%</h3><canvas id='c0'></canvas></div>"
                        "<div class='card'><h3>PWM 4-Channels Output</h3><canvas id='c1'></canvas></div>"
                        "<div class='card'><h3>Encoder 2-Channels Input</h3><canvas id='c2'></canvas></div>"

                        "<div class='card'><h3>GPIO Control</h3>"
                        // --- PHỤC HỒI GIAO DIỆN CŨ CHO TRIGGERS ---
                        "  <div class='btn-group'><span>Trigger 1 (Pin 54)</span><div><button id='b0_1' onclick='set(0,1)'>ON</button><button id='b0_0' onclick='set(0,0)'>OFF</button></div></div>"
                        "  <div class='btn-group'><span>Trigger 2 (Pin 55)</span><div><button id='b1_1' onclick='set(1,1)'>ON</button><button id='b1_0' onclick='set(1,0)'>OFF</button></div></div>"
                        // --- HIỂN THỊ TRẠNG THÁI LED ---
                        "  <div class='btn-group'><span>LED 0 (Heartbeat)</span><span id='l0' class='led led-blue'></span></div>"
                        "  <div class='btn-group'><span>LED 1 (Web Activity)</span><span id='l1' class='led led-blue'></span></div>"
                        "</div>"

                        "<div class='card' style='grid-column: span 2'><h3>Industrial Inputs (EMIO)</h3>"
                        "  <div style='display:flex; justify-content:space-around; background:#2d2d2d; padding:15px; border-radius:10px'>"
                        "    <div><span id='in0' class='led led-red'></span>IN 0</div><div><span id='in1' class='led led-red'></span>IN 1</div>"
                        "    <div><span id='in2' class='led led-red'></span>IN 2</div><div><span id='in3' class='led led-red'></span>IN 3</div>"
                        "  </div>"
                        "</div>"

                        "<div class='card' style='grid-column: span 2'><h3>UART Monitor</h3>"
                        "  <table><thead><tr><th>Port</th><th>Last RX</th><th>Last TX</th></tr></thead><tbody id='utb'></tbody></table>"
                        "</div>"

                        "<script>"
                        "const fC=(id,ls,cs)=>new Chart(id,{type:'line',data:{labels:[],datasets:ls.map((l,i)=>({label:l,data:[],borderColor:cs[i],tension:0.2,pointRadius:0}))},options:{animation:false,scales:{y:{grid:{color:'#333'}}}}});"
                        "let charts; window.onload=()=>{ "
                        "  charts=[fC('c0',['CPU'],['#0f0']),fC('c1',['CH1','CH2','CH3','CH4'],['#f00','#0f0','#0af','#ff0']),fC('c2',['ENC_A','ENC_B'],['#f0f','#fff'])]; "
                        "  setInterval(upd,1000); "
                        "};"
                        "function set(p,v){ fetch(`/set?p=${p}&v=${v}`); }"
                        "function upd(){ fetch('/data').then(r=>r.json()).then(d=>{"
                        "  document.getElementById('cpv').innerText=d.cpu; const now=new Date().toLocaleTimeString();"
                        "  [ [d.cpu],d.pwm,d.enc ].forEach((v,i)=>{ charts[i].data.labels.push(now); v.forEach((val,j)=>charts[i].data.datasets[j].data.push(val));"
                        "  if(charts[i].data.labels.length>20){charts[i].data.labels.shift(); charts[i].data.datasets.forEach(ds=>ds.data.shift());} charts[i].update(); });"
                        /* Cập nhật đèn LED và trạng thái nút bấm */
                        "  for(let i=0;i<4;i++) document.getElementById('in'+i).className = d.gpio_in[i]?'led led-red on':'led led-red';"
                        "  document.getElementById('l0').className=d.gpio_out[2]?'led led-blue on':'led led-blue';"
                        "  document.getElementById('l1').className=d.gpio_out[3]?'led led-blue on':'led led-blue';"
                        "  document.getElementById('b0_1').className = d.gpio_out[0]?'active':''; document.getElementById('b0_0').className = !d.gpio_out[0]?'active':'';"
                        "  document.getElementById('b1_1').className = d.gpio_out[1]?'active':''; document.getElementById('b1_0').className = !d.gpio_out[1]?'active':'';"
                        "  document.getElementById('utb').innerHTML=d.uarts.map((u,i)=>`<tr><td><b>UART ${i+1}</b></td><td style='color:#0f0'>${u.rx}</td><td style='color:#0af'>${u.tx}</td></tr>`).join('');"
                        "}); }"
                        "</script></body></html>";
                write(cfd, html, strlen(html));
            }
        }
        close(cfd);

        // TẮT LED 1 sau khi xử lý xong (tạo hiệu ứng nhấp nháy cực nhanh)
        usleep(50000); // Giữ đèn sáng 50ms để mắt kịp thấy
        gpio_op(out_pins[3], "value", "0");
        gpio_out[3] = 0;
    }
}

// --- 5. Luồng UDP (Batching TX) ---
void* udp_tx_worker(void* arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in rem = { .sin_family=AF_INET, .sin_port=htons(UDP_TX_PORT) };
    inet_pton(AF_INET, REMOTE_IP, &rem.sin_addr);
    struct mmsghdr msgs[4]; struct iovec iovs[4]; char b[4][128];
    memset(msgs, 0, sizeof(msgs));
    for(int i=0; i<4; i++){ iovs[i].iov_base=b[i]; iovs[i].iov_len=128; msgs[i].msg_hdr.msg_iov=&iovs[i]; msgs[i].msg_hdr.msg_iovlen=1; msgs[i].msg_hdr.msg_name=&rem; msgs[i].msg_hdr.msg_namelen=sizeof(rem); }
    while(1) {
        for(int i=0; i<4; i++) sprintf(b[i], "STATUS: PWM=%u ENC=%u", pwm.ptr[0], enc.ptr[0]);
        sendmmsg(sock, msgs, 4, 0); usleep(1000000);
    }
}

int main() {
    pthread_t t[16];
    
    // Khởi tạo UIO
    int f0 = open("/dev/uio0", O_RDWR); pwm.ptr = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, f0, 0);
    int f1 = open("/dev/uio1", O_RDWR); enc.ptr = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, f1, 0);
    
    // Xuất bản và cấu hình hướng cho GPIO (EMIO 54-57 và 69-72)
    char cmd[128];
    for(int i=0; i<4; i++) { 
        sprintf(cmd, "echo %d > /sys/class/gpio/export 2>/dev/null", out_pins[i]+GPIO_BASE); system(cmd);
        sprintf(cmd, "echo out > /sys/class/gpio/gpio%d/direction", out_pins[i]+GPIO_BASE); system(cmd);
        sprintf(cmd, "echo %d > /sys/class/gpio/export 2>/dev/null", in_pins[i]+GPIO_BASE); system(cmd);
        sprintf(cmd, "echo in > /sys/class/gpio/gpio%d/direction", in_pins[i]+GPIO_BASE); system(cmd);
    }

    // Khởi tạo luồng
    pthread_create(&t[0], NULL, led_heartbeat_worker, NULL); // LED0 nhấp nháy
    pthread_create(&t[1], NULL, web_worker, NULL);           // LED1 nhấp nháy ở đây
    pthread_create(&t[2], NULL, udp_tx_worker, NULL);

    for(int i=0; i<UART_COUNT; i++) {
        u_info[i].id=i+1; sprintf(u_info[i].path, "/dev/ttyUL%d", i+1);
        strcpy(u_info[i].last_rx, "-"); strcpy(u_info[i].last_tx, "-");
        pthread_mutex_init(&u_info[i].lock, NULL);
        pthread_create(&t[i+3], NULL, uart_worker, &u_info[i]);
    }

    printf("Hệ thống Ebaz4205 v7 Sẵn sàng! LED0: Heartbeat, LED1: Web Activity\n");
    
    while(1) { // Đọc Input định kỳ
        char path[64], val;
        for(int i=0; i<4; i++) {
            sprintf(path, "/sys/class/gpio/gpio%d/value", in_pins[i] + GPIO_BASE);
            int fd = open(path, O_RDONLY); if(fd>=0) { read(fd, &val, 1); gpio_in[i] = (val=='1'); close(fd); }
        }
        usleep(200000);
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
// #define REMOTE_IP       "192.168.1.123"
// #define UDP_TX_PORT     2027
// #define UDP_RX_PORT     2026

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
// int out_pins[] = {54, 55, 56, 57}; // Trig1,2, Led0,1
// int in_pins[]  = {69, 70, 71, 72}; // Input0-3

// // --- Hàm bổ trợ: Làm sạch chuỗi cho JSON ---
// void clean_str(char *s) {
//     for (; *s; s++) if (!isprint(*s) || *s == '"' || *s == '\\') *s = ' ';
// }

// // --- Hàm GPIO ---
// void gpio_op(int pin, char *file, char *val) {
//     char p[64]; sprintf(p, "/sys/class/gpio/gpio%d/%s", pin + GPIO_BASE, file);
//     int fd = open(p, O_WRONLY); if(fd >= 0) { write(fd, val, strlen(val)); close(fd); }
// }

// // --- 1. Luồng CPU ---
// void* cpu_worker(void* arg) {
//     long double a[4], b[4];
//     while(1) {
//         FILE *fp = fopen("/proc/stat", "r");
//         fscanf(fp, "%*s %Lf %Lf %Lf %Lf", &a[0], &a[1], &a[2], &a[3]); fclose(fp);
//         usleep(500000);
//         fp = fopen("/proc/stat", "r");
//         fscanf(fp, "%*s %Lf %Lf %Lf %Lf", &b[0], &b[1], &b[2], &b[3]); fclose(fp);
//         cpu_val = ((b[0]+b[1]+b[2]) - (a[0]+a[1]+a[2])) / ((b[0]+b[1]+b[2]+b[3]) - (a[0]+a[1]+a[2]+a[3])) * 100;
//     }
// }

// // --- 2. Luồng UART ---
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

// // --- Hàm tính toán % CPU từ hệ thống ---
// void update_cpu() {
//     static long long last_user, last_nice, last_system, last_idle, last_iowait, last_irq, last_softirq;
//     long long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;

//     FILE *fp = fopen("/proc/stat", "r");
//     if (fp == NULL) return;

//     char line[256];
//     // Đọc dòng đầu tiên bắt đầu bằng "cpu "
//     if (fgets(line, sizeof(line), fp)) {
//         sscanf(line, "cpu %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld",
//                &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice);
//     }
//     fclose(fp);

//     // Tính toán tổng thời gian rảnh và thời gian hoạt động
//     long long prev_idle = last_idle + last_iowait;
//     long long current_idle = idle + iowait;

//     long long prev_non_idle = last_user + last_nice + last_system + last_irq + last_softirq;
//     long long current_non_idle = user + nice + system + irq + softirq;

//     long long prev_total = prev_idle + prev_non_idle;
//     long long current_total = current_idle + current_non_idle;

//     long long total_diff = current_total - prev_total;
//     long long idle_diff = current_idle - prev_idle;

//     // Tránh chia cho 0 trong lần chạy đầu tiên
//     if (total_diff > 0) {
//         cpu_val = (float)(total_diff - idle_diff) / total_diff * 100.0;
//     }

//     // Lưu lại giá trị cho lần tính sau
//     last_user = user; last_nice = nice; last_system = system;
//     last_idle = idle; last_iowait = iowait; last_irq = irq; last_softirq = softirq;
// }

// void* web_worker(void* arg) {
//     int sfd = socket(AF_INET, SOCK_STREAM, 0);
//     int opt = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
//     struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(WEB_PORT), .sin_addr.s_addr = INADDR_ANY };
//     bind(sfd, (struct sockaddr *)&addr, sizeof(addr)); listen(sfd, 10);

//     printf("[OK] Web Server Pro Online tại cổng %d\n", WEB_PORT);

//     while(1) {
//         int cfd = accept(sfd, NULL, NULL);
//         char req[1024]; int rlen = read(cfd, req, 1023);
//         if (rlen <= 0) { close(cfd); continue; }
//         req[rlen] = 0;

//         // --- Xử lý điều khiển GPIO ---
//         if (strstr(req, "GET /set")) {
//             int p, v;
//             if (sscanf(strstr(req, "p="), "p=%d&v=%d", &p, &v) == 2) {
//                 if(p>=0 && p<4) { 
//                     char vs[2]; sprintf(vs,"%d",v); 
//                     gpio_op(out_pins[p], "value", vs); 
//                     gpio_out[p]=v; 
//                 }
//             }
//         }
        
//         // --- Trả về dữ liệu JSON ---
//         if (strstr(req, "GET /data")) {
//             update_cpu();
//             char json[8192];
//             int p = sprintf(json, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n"
//                                   "{\"cpu\":%.1f, \"pwm\":[%u,%u,%u,%u], \"enc\":[%u,%u], \"gpio_in\":[%d,%d,%d,%d], \"gpio_out\":[%d,%d,%d,%d], \"uarts\":[",
//                                   cpu_val, pwm.ptr[0], pwm.ptr[1], pwm.ptr[2], pwm.ptr[3], enc.ptr[0], enc.ptr[1],
//                                   gpio_in[0], gpio_in[1], gpio_in[2], gpio_in[3],
//                                   gpio_out[0], gpio_out[1], gpio_out[2], gpio_out[3]);
            
//             for(int i=0; i<UART_COUNT; i++) {
//                 pthread_mutex_lock(&u_info[i].lock);
//                 p += sprintf(json+p, "{\"rx\":\"%s\",\"tx\":\"%s\"}%s", 
//                              u_info[i].last_rx, u_info[i].last_tx, (i == (UART_COUNT-1) ? "" : ","));
//                 pthread_mutex_unlock(&u_info[i].lock);
//             }
//             sprintf(json+p, "]}");
//             write(cfd, json, strlen(json));
//         } 
//         // --- Trả về giao diện HTML/CSS/JS ---
//         else {
//             const char *html = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
//                 "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Ebaz4205 Pro</title>"
//                 "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>"
//                 "<style>"
//                 "body{background:#111;color:#eee;font-family:sans-serif;display:grid;grid-template-columns:1fr 1fr;gap:15px;padding:15px}"
//                 ".card{background:#222;padding:15px;border-radius:12px;border:1px solid #444;box-shadow:0 4px 10px rgba(0,0,0,0.5)}"
//                 "canvas{max-height:180px} table{width:100%;border-collapse:collapse;font-size:13px;margin-top:10px}"
//                 "th,td{border:1px solid #333;padding:8px;text-align:left} th{background:#333;color:#aaa}"
//                 ".led{width:14px;height:14px;border-radius:50%;display:inline-block;vertical-align:middle;margin-right:8px;border:1px solid #444}"
//                 ".led-red{background:#500}.led-red.on{background:#f00;box-shadow:0 0 10px #f00}"
//                 ".led-green{background:#050}.led-green.on{background:#0f0;box-shadow:0 0 10px #0f0}"
//                 ".led-blue{background:#005}.led-blue.on{background:#0af;box-shadow:0 0 10px #0af}"
//                 ".btn-group{margin-bottom:10px;display:flex;align-items:center;justify-content:space-between;background:#2d2d2d;padding:8px;border-radius:8px}"
//                 "button{background:#444;color:white;border:none;padding:6px 12px;border-radius:4px;cursor:pointer;margin-left:5px}"
//                 "button:hover{background:#666} .active{background:#05f!important}"
//                 "</style></head><body>"
                
//                 "<div class='card'><h3><span class='led led-green on'></span>CPU: <span id='cpv'>0</span>%</h3><canvas id='c0'></canvas></div>"
//                 "<div class='card'><h3>PWM 4-Channels Output</h3><canvas id='c1'></canvas></div>"
//                 "<div class='card'><h3>Encoder 2-Channels Input</h3><canvas id='c2'></canvas></div>"

//                 "<div class='card'><h3>GPIO Control</h3>"
//                 "  <div class='btn-group'><span>Trigger 1 (P54)</span><div><button id='b0_1' onclick='set(0,1)'>ON</button><button id='b0_0' onclick='set(0,0)'>OFF</button></div></div>"
//                 "  <div class='btn-group'><span>Trigger 2 (P55)</span><div><button id='b1_1' onclick='set(1,1)'>ON</button><button id='b1_0' onclick='set(1,0)'>OFF</button></div></div>"
//                 "  <div class='btn-group'><span>LED 0 (P56)</span><span id='l0' class='led led-blue'></span></div>"
//                 "  <div class='btn-group'><span>LED 1 (P57)</span><span id='l1' class='led led-blue'></span></div>"
//                 "</div>"

//                 "<div class='card' style='grid-column: span 2'><h3>Industrial Inputs</h3>"
//                 "  <div style='display:flex; justify-content:space-around; background:#2d2d2d; padding:15px; border-radius:10px'>"
//                 "    <div><span id='in0' class='led led-red'></span>IN 0</div><div><span id='in1' class='led led-red'></span>IN 1</div>"
//                 "    <div><span id='in2' class='led led-red'></span>IN 2</div><div><span id='in3' class='led led-red'></span>IN 3</div>"
//                 "  </div>"
//                 "</div>"

//                 "<div class='card' style='grid-column: span 2'><h3>UART Monitor (10 Ports)</h3>"
//                 "  <table><thead><tr><th>Port</th><th>Last RX</th><th>Last TX</th></tr></thead><tbody id='utb'></tbody></table>"
//                 "</div>"

//                 "<script>"
//                 "const fC = (id,ls,cs) => new Chart(id,{type:'line',data:{labels:[],datasets:ls.map((l,i)=>({label:l,data:[],borderColor:cs[i],tension:0.2,pointRadius:0}))},options:{animation:false,scales:{y:{grid:{color:'#333'}}}}});"
//                 "let charts; window.onload=()=>{ "
//                 "  charts=[fC('c0',['CPU'],['#0f0']),fC('c1',['CH1','CH2','CH3','CH4'],['#f00','#0f0','#0af','#ff0']),fC('c2',['ENC_A','ENC_B'],['#f0f','#fff'])]; "
//                 "  setInterval(upd,1000); "
//                 "};"
//                 "function set(p,v){ fetch(`/set?p=${p}&v=${v}`); }"
//                 "function upd(){ fetch('/data').then(r=>r.json()).then(d=>{"
//                 "  document.getElementById('cpv').innerText=d.cpu; const now=new Date().toLocaleTimeString();"
//                 "  [ [d.cpu],d.pwm,d.enc ].forEach((v,i)=>{ charts[i].data.labels.push(now); v.forEach((val,j)=>charts[i].data.datasets[j].data.push(val));"
//                 "  if(charts[i].data.labels.length>20){charts[i].data.labels.shift(); charts[i].data.datasets.forEach(ds=>ds.data.shift());} charts[i].update(); });"
//                 "  for(let i=0;i<4;i++) document.getElementById('in'+i).className = d.gpio_in[i]?'led led-red on':'led led-red';"
//                 "  document.getElementById('l0').className = d.gpio_out[2]?'led led-blue on':'led led-blue';"
//                 "  document.getElementById('l1').className = d.gpio_out[3]?'led led-blue on':'led led-blue';"
//                 "  document.getElementById('b0_1').className = d.gpio_out[0]?'active':''; document.getElementById('b0_0').className = !d.gpio_out[0]?'active':'';"
//                 "  document.getElementById('b1_1').className = d.gpio_out[1]?'active':''; document.getElementById('b1_0').className = !d.gpio_out[1]?'active':'';"
//                 "  let h=''; d.uarts.forEach((u,i)=>{ h+=`<tr><td><b>UART ${i+1}</b></td><td style='color:#0f0;font-family:monospace'>${u.rx}</td><td style='color:#0af;font-family:monospace'>${u.tx}</td></tr>`; });"
//                 "  document.getElementById('utb').innerHTML=h;"
//                 "}).catch(e=>console.log('Error:',e)); }"
//                 "</script></body></html>";
//             write(cfd, html, strlen(html));
//         }
//         close(cfd);
//     }
// }

// // --- 4. Luồng UDP (Batching TX) ---
// void* udp_tx_worker(void* arg) {
//     int sock = socket(AF_INET, SOCK_DGRAM, 0);
//     struct sockaddr_in rem = { .sin_family=AF_INET, .sin_port=htons(UDP_TX_PORT) };
//     inet_pton(AF_INET, REMOTE_IP, &rem.sin_addr);
//     struct mmsghdr msgs[4]; struct iovec iovs[4]; char b[4][128];
//     memset(msgs, 0, sizeof(msgs));
//     for(int i=0; i<4; i++){ iovs[i].iov_base=b[i]; iovs[i].iov_len=128; msgs[i].msg_hdr.msg_iov=&iovs[i]; msgs[i].msg_hdr.msg_iovlen=1; msgs[i].msg_hdr.msg_name=&rem; msgs[i].msg_hdr.msg_namelen=sizeof(rem); }
//     while(1) {
//         for(int i=0; i<4; i++) sprintf(b[i], "STATUS: CPU=%.1f PWM=%u ENC=%u", cpu_val, pwm.ptr[0], enc.ptr[0]);
//         sendmmsg(sock, msgs, 4, 0); usleep(1000000);
//     }
// }

// int main() {
//     pthread_t t[15];
//     int f0 = open("/dev/uio0", O_RDWR); pwm.ptr = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, f0, 0);
//     int f1 = open("/dev/uio1", O_RDWR); enc.ptr = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, f1, 0);
    
//     pthread_create(&t[0], NULL, cpu_worker, NULL);
//     pthread_create(&t[1], NULL, web_worker, NULL);
//     pthread_create(&t[2], NULL, udp_tx_worker, NULL);

//     for(int i=0; i<UART_COUNT; i++) {
//         u_info[i].id=i+1; sprintf(u_info[i].path, "/dev/ttyUL%d", i+1);
//         strcpy(u_info[i].last_rx, "-"); strcpy(u_info[i].last_tx, "-");
//         pthread_mutex_init(&u_info[i].lock, NULL);
//         pthread_create(&t[i+3], NULL, uart_worker, &u_info[i]);
//     }

//     printf("Hệ thống Ebaz4205 v6 Online: http://192.168.1.6\n");
//     while(1) { // Luồng đọc GPIO Input định kỳ
//         char path[64], val;
//         for(int i=0; i<4; i++) {
//             sprintf(path, "/sys/class/gpio/gpio%d/value", in_pins[i] + GPIO_BASE);
//             int fd = open(path, O_RDONLY); if(fd>=0) { read(fd, &val, 1); gpio_in[i] = (val=='1'); close(fd); }
//         }

        
//         usleep(200000);
//     }
//     return 0;
// }