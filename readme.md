Hướng Dẫn Cấu Hình và Biên Dịch Dự Án C Cross-Compile cho ARM trên WSL & VS Code
lênh build: 
make
make clean

trên terminal, đánh lệnh load trình biên dịch:
source ~/ebaz_sdk/environment-setup-cortexa9t2hf-neon-xilinx-linux-gnueabi

copy xuống kit:
root@xilinx-zcu216-2021_2:~# scp main_app root@192.168.1.36:/home/root/                 
The authenticity of host '192.168.1.36 (192.168.1.36)' can't be established.
ECDSA key fingerprint is SHA256:5P/PK5RCDKw430K5TT3uaD6JmWuThdNZkx66Ns6/Re4.
Are you sure you want to continue connecting (yes/no/[fingerprint])? yes
Warning: Permanently added '192.168.1.56' (ECDSA) to the list of known hosts.
root@192.168.1.36's password: r
main_app                                                                                                                                                                  100% 2158KB  33.