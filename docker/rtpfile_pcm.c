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
int total_pcm_bytes = 0;
int running = 1;
char output_filename[256] = "audio.bin";

// RTP Header 结构体定义
typedef struct {
    unsigned char csrc_len:4;
    unsigned char extension:1;
    unsigned char padding:1;
    unsigned char version:2;
    unsigned char payload_type:7;
    unsigned char marker:1;
    unsigned short seq_no;
    unsigned int timestamp;
    unsigned int ssrc;
} rtp_header_t;

// WAV 文件头结构体
typedef struct {
    char chunk_id[4];
    unsigned int chunk_size;
    char format[4];
    char subchunk1_id[4];
    unsigned int subchunk1_size;
    unsigned short audio_format;
    unsigned short num_channels;
    unsigned int sample_rate;
    unsigned int byte_rate;
    unsigned short block_align;
    unsigned short bits_per_sample;
    char subchunk2_id[4];
    unsigned int subchunk2_size;
} wav_header_t;

// 端序转换函数：16-bit 大端转小端
void convert_be_to_le(unsigned char *data, int len) {
    for (int i = 0; i < len; i += 2) {
        unsigned char tmp = data[i];
        data[i] = data[i + 1];
        data[i + 1] = tmp;
    }
}

// 写入 WAV 文件头的函数
void write_wav_header() {
    if (output_file == NULL) return;
    
    wav_header_t wav_hdr;
    
    // 填充 WAV 头
    memcpy(wav_hdr.chunk_id, "RIFF", 4);
    wav_hdr.chunk_size = 36 + total_pcm_bytes;
    memcpy(wav_hdr.format, "WAVE", 4);
    memcpy(wav_hdr.subchunk1_id, "fmt ", 4);
    wav_hdr.subchunk1_size = 16;
    wav_hdr.audio_format = 1;      // PCM 格式
    wav_hdr.num_channels = 1;      // 单声道
    wav_hdr.sample_rate = 8000;    // 8kHz
    wav_hdr.bits_per_sample = 16;  // 16-bit
    wav_hdr.byte_rate = wav_hdr.sample_rate * wav_hdr.num_channels * wav_hdr.bits_per_sample / 8;
    wav_hdr.block_align = wav_hdr.num_channels * wav_hdr.bits_per_sample / 8;
    memcpy(wav_hdr.subchunk2_id, "data", 4);
    wav_hdr.subchunk2_size = total_pcm_bytes;
    
    // 移动到文件开头，写入 WAV 头
    rewind(output_file);
    fwrite(&wav_hdr, sizeof(wav_hdr), 1, output_file);
    fflush(output_file);
    
    printf("\n========================================\n");
    printf("File saved: %s\n", output_filename);
    printf("Total PCM bytes: %d bytes\n", total_pcm_bytes);
    printf("Duration: %.2f seconds\n", (float)total_pcm_bytes / (8000 * 2));
    printf("========================================\n");
}

// 信号处理函数
void signal_handler(int signum) {
    printf("\n\nReceived signal %d. Saving file and exiting...\n", signum);
    running = 0;
    if (sockfd != -1) {
        shutdown(sockfd, SHUT_RD);
    }
}

int main(int argc, char *argv[]) {
    struct sockaddr_in servaddr;
    char buffer[2048];
    rtp_header_t *rtp_hdr;
    unsigned short expected_seq = 0;
    int first_packet = 1;
    
    // 如果命令行指定了文件名，使用它
    if (argc > 1) {
        strcpy(output_filename, argv[1]);
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
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
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
    printf("RTP Receiver for Cool Edit Pro\n");
    printf("Listening on port 5000...\n");
    printf("Output file: %s\n", output_filename);
    printf("Press Ctrl+C to stop and save file\n");
    printf("========================================\n\n");
    
    output_file = fopen(output_filename, "wb");
    if (!output_file) {
        perror("File open failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
   
    int show_codec_flag=0;
    
    // 2. 接收并处理 RTP 包
    while (running) {
        int len = recv(sockfd, buffer, sizeof(buffer), 0);
        
        if (len < 0) {
            // 超时或错误，继续循环检查 running 标志
            continue;
        }
        
        if (len < 12) {
            continue; // 无效包
        }
        
        rtp_hdr = (rtp_header_t *)buffer;
        
        // 验证 RTP 版本
        if (rtp_hdr->version != 2) {
            continue;
        }
        
        // 获取载荷数据指针 (跳过 RTP 头)
        char *payload = buffer + sizeof(rtp_header_t);
        int payload_len = len - sizeof(rtp_header_t);
        
        if (payload_len <= 0) {
            continue;
        }
        
        // 处理序列号，检测丢包
        if (!first_packet) {
            unsigned short diff = ntohs(rtp_hdr->seq_no) - expected_seq;
            if (diff != 1 && diff != 0 && diff < 100) {
                printf("\nPacket loss detected! Expected %d, got %d\n", 
                       expected_seq, ntohs(rtp_hdr->seq_no));
                // 插入静音来填充丢包
                if (diff > 1) {
                    int silence_samples = diff * payload_len;
                    short *silence = calloc(silence_samples / 2, 2);
                    fwrite(silence, 1, silence_samples, output_file);
                    total_pcm_bytes += silence_samples;
                    free(silence);
                    printf("Inserted %d bytes of silence\n", silence_samples);
                }
            }
        } else {
            first_packet = 0;
            printf("First packet received. Sequence: %d\n", ntohs(rtp_hdr->seq_no));
        }
        expected_seq = ntohs(rtp_hdr->seq_no) + 1;
        
        if(show_codec_flag==0)
        {
		if(rtp_hdr->payload_type==0)
			printf("rtp_hdr->payload_type==0 is u-law rtp\n");
        	else if(rtp_hdr->payload_type==8)
			printf("rtp_hdr->payload_type==8 is a-law rtp\n");
        	else if(rtp_hdr->payload_type==96)
			printf("rtp_hdr->payload_type==96 is opus rtp\n");
        	else
			printf("rtp_hdr->payload_type==%u\n",rtp_hdr->payload_type);
		show_codec_flag=1;
	}
        
        
        // ========== 关键修改：端序转换 ==========
        // RTP 载荷是大端序，转换为小端序再保存
        //convert_be_to_le((unsigned char *)payload, payload_len);
        // ====================================
        
        // 写入转换后的 PCM 数据
        fwrite(payload, 1, payload_len, output_file);
        total_pcm_bytes += payload_len;
        
        // 每 100 个包打印一次状态
        if (ntohs(rtp_hdr->seq_no) % 100 == 0) {
            printf("Received %d packets, %d bytes (%d KB)\r", 
                   ntohs(rtp_hdr->seq_no), total_pcm_bytes, total_pcm_bytes / 1024);
            fflush(stdout);
        }
    }
    
    // 3. 程序退出前写入 WAV 头
    printf("\n\nStopping receiver...\n");
    //write_wav_header();
    
    // 清理资源
    fclose(output_file);
    close(sockfd);
    
    printf("\nDone! File '%s' is ready for Cool Edit Pro.\n", output_filename);
    
    return 0;
}

