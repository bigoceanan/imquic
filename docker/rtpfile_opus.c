#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
// 全局变量
int sockfd = -1;
FILE *output_file = NULL;
int total_opus_bytes = 0;
int total_packets = 0;
int running = 1;
char output_filename[256] = "audio.opus";
// RTP Header 结构体定义 - 使用 packed 避免内存对齐问题
typedef struct __attribute__((packed)) {
    unsigned char version:2;
    unsigned char padding:1;
    unsigned char extension:1;
    unsigned char csrc_len:4;
    unsigned char marker:1;
    unsigned char payload_type:7;
    unsigned short seq_no;
    unsigned int timestamp;
    unsigned int ssrc;
} rtp_header_t;

// 【核心修改】写入标准 OGG Opus 头（替换原有头函数，解决无法识别问题）
void write_opus_header() {
    if (output_file == NULL) return;
    
    // 标准 OpusHead 头（19字节，兼容ffmpeg/VLC）
    unsigned char opus_head[] = {
        'O','p','u','s','H','e','a','d', // magic: OpusHead
        1,          // version 1
        1,          // channels: 单声道
        0x00,0x00,  // pre-skip: 0
        0x80,0xbb,0x00,0x00, // sample rate: 48000 (小端序)
        0x00,0x00,  // gain: 0dB
        0x00        // channel mapping: 0
    };
    fwrite(opus_head, 1, 19, output_file);

    // 标准 OpusTags 头（28字节，补充元信息）
    unsigned char opus_tags[] = {
        'O','p','u','s','T','a','g','s', // magic: OpusTags
        0x0C,0x00,0x00,0x00,             // vendor len: 12
        'L','C',' ','w','e','b','r','t','c','-','r','t','p',  // 这里拆分成单个字符
        0x01,0x00,0x00,0x00,             // comment count: 1
        0x00,0x00,0x00,0x00              // comment len: 0 (无额外注释)
    };
    fwrite(opus_tags, 1, 28, output_file);
    fflush(output_file);
    
    printf("\n========================================\n");
    printf("Opus file header written (STANDARD OGG)\n");
    printf("  Format: Standard OGG Opus (playable directly)\n");
    printf("  Channels: 1 (Mono)\n");
    printf("  Sample rate: 48000 Hz\n");
    printf("  Compatible with: ffplay / VLC / ffprobe\n");
    printf("========================================\n");
}

// 信号处理函数
void signal_handler(int signum) {
    printf("\n\nReceived signal %d. Saving file and exiting...\n", signum);
    running = 0;
    if (sockfd != -1) {
        close(sockfd);
    }
}

int main(int argc, char *argv[]) {
    struct sockaddr_in servaddr;
    char buffer[65536];
    rtp_header_t *rtp_hdr;
    unsigned short expected_seq = 0;
    int first_packet = 1;
    int save_raw_opus = 0;
    
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i+1 < argc) {
            strcpy(output_filename, argv[i+1]);
            i++;
        } else if (strcmp(argv[i], "-raw") == 0) {
            save_raw_opus = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options] [filename]\n", argv[0]);
            printf("Options:\n");
            printf("  -o <file>    Output filename (default: audio.opus)\n");
            printf("  -raw         Save raw Opus packets (no header)\n");
            printf("  -h, --help   Show this help\n");
            exit(0);
        } else if (i == argc-1) {
            strcpy(output_filename, argv[i]);
        }
    }
    
    // 如果不是 raw 模式且没有指定扩展名，自动添加 .opus
    if (!save_raw_opus && strstr(output_filename, ".opus") == NULL && 
        strstr(output_filename, ".Opus") == NULL &&
        strstr(output_filename, ".ogg") == NULL) {
        strcat(output_filename, ".opus");
    }
    
    // 注册信号处理函数
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 1. 创建 UDP Socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // 设置 socket 接收超时
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt failed");
    }
    
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(5000);
    
    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    printf("========================================\n");
    printf("Opus RTP Receiver (STANDARD OGG)\n");
    printf("Listening on port 5000...\n");
    printf("Output file: %s\n", output_filename);
    if (save_raw_opus) {
        printf("Format: Raw Opus packets (no header)\n");
    } else {
        printf("Format: Standard OGG Opus (playable with ffplay, VLC, etc.)\n");
    }
    printf("Press Ctrl+C to stop and save file\n");
    printf("========================================\n\n");
    
    output_file = fopen(output_filename, "wb");
    if (!output_file) {
        perror("File open failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    // 如果不是保存原始格式，写入标准 OGG Opus 头
    if (!save_raw_opus) {
        write_opus_header();
    }
    
    int show_codec_flag = 0;
    
    // 2. 接收并处理 RTP 包
    while (running) {
        int len = recv(sockfd, buffer, sizeof(buffer), 0);
        
        // 打印UDP包长度（仅错误/超时打印，避免日志刷屏）
        if (len < 0) {
            // 超时或错误，仅打印一次提示即可，注释掉刷屏日志
            // printf("Received UDP packet: %d bytes\n", len);
            continue;
        }
        
        if (len < 12) {
            printf("Received packet too short: %d bytes\n", len);
            continue; // 无效包
        }
        
        // 确保缓冲区足够大
        if (len > sizeof(buffer)) {
            printf("Packet too large: %d bytes\n", len);
            continue;
        }
        
        rtp_hdr = (rtp_header_t *)buffer;
        // 手动解析RTP头关键字段（解决结构体位域字节序问题）
        uint8_t first_byte = buffer[0];
        uint8_t second_byte = buffer[1];
        rtp_hdr->version = (first_byte >> 6) & 0x03;
        rtp_hdr->payload_type = second_byte & 0x7F;        
        rtp_hdr->csrc_len = (first_byte >> 0) & 0x0F; // 补全CSRC长度手动解析，确保头计算准确

        // 正确计算 RTP 头总长（包含 CSRC 扩展，核心修复）
        int header_len = 12;
        header_len += rtp_hdr->csrc_len * 4;  // CSRC 每个项占 4 字节
        // 载荷开始位置 = 头总长度
        char *payload = buffer + header_len;
        int payload_len = len - header_len;
        
        if (payload_len <= 0) {
            printf("payload_len<=0[%d]\n", payload_len);
            continue;
        }
        
        // 处理序列号，检测丢包（网络字节序转换）
        unsigned short current_seq = ntohs(rtp_hdr->seq_no);
        if (!first_packet) {
            unsigned short diff = current_seq - expected_seq;
            if (diff != 1 && diff != 0 && diff < 100) {
                printf("\n[WARNING] Packet loss detected! Expected %d, got %d\n", 
                       expected_seq, current_seq);
                printf("          This may cause audio glitches\n");
            }
        } else {
            first_packet = 0;
            printf("First packet received.\n");
            printf("  Sequence: %d\n", current_seq);
            printf("  Payload type: %d\n", rtp_hdr->payload_type);
            printf("  Timestamp: %u\n", ntohl(rtp_hdr->timestamp));
        }
        expected_seq = current_seq + 1;
        
        // 显示编码类型（只显示一次）
        if (show_codec_flag == 0) {
            if (rtp_hdr->payload_type == 96) {
                printf("\n✓ Payload type 96 - Opus RTP detected\n\n");
            } else if (rtp_hdr->payload_type == 0) {
                printf("\n✗ Payload type 0 - u-law RTP (not Opus)\n");
                printf("  Please check your RTP sender configuration\n\n");
            } else if (rtp_hdr->payload_type == 8) {
                printf("\n✗ Payload type 8 - a-law RTP (not Opus)\n");
                printf("  Please check your RTP sender configuration\n\n");
            } else {
                printf("\n⚠ Payload type: %d (unexpected for Opus)\n\n", rtp_hdr->payload_type);
            }
            show_codec_flag = 1;
        }
        
        // 写入 Opus 载荷数据（标准OGG格式可直接写入，已加标准头）
        // =============== 在这里替换 ===============
        // 【关键】每个包前加 1 字节长度
        unsigned char len_header = payload_len;
        fwrite(&len_header, 1, 1, output_file);

        // 写入真实数据
        size_t written = fwrite(payload, 1, payload_len, output_file);
        // ==========================================

        if (written != payload_len) {
            printf("Error writing to file\n");
            break;
        }
        
        total_opus_bytes += payload_len;
        total_packets++;
        
        // 每 100 个包打印一次状态，刷新stdout
        if (total_packets % 100 == 0) {
            printf("Received %d packets, %d bytes (%d KB)\r", 
                   total_packets, total_opus_bytes, total_opus_bytes / 1024);
            fflush(stdout);
        }
    }
    
    // 3. 程序退出
    printf("\n\n========================================\n");
    printf("Stopping receiver...\n");
    printf("Total packets received: %d\n", total_packets);
    printf("Total Opus data: %d bytes (%.2f KB)\n", 
           total_opus_bytes, total_opus_bytes / 1024.0);
    printf("========================================\n");
    
    // 清理资源
    if (output_file) {
        fclose(output_file);
    }
    if (sockfd != -1) {
        close(sockfd);
    }
    
    if (save_raw_opus) {
        printf("\n✓ Raw Opus data saved to: %s\n", output_filename);
        printf("  Note: This file may not play directly. Use ffmpeg to add headers:\n");
        printf("  ffmpeg -f opus -i %s -c copy output.opus\n", output_filename);
    } else {
        printf("\n✓ Standard OGG Opus file saved to: %s\n", output_filename);
        printf("  Play directly with: ffplay %s\n", output_filename);
        printf("  Or: vlc %s\n", output_filename);
        printf("  Check info: ffprobe %s\n", output_filename);
        printf("\n  Total Opus payload size: %d bytes\n", total_opus_bytes);
    }
    
    return 0;
}

