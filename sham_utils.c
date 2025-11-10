#include "sham.h"
#include <openssl/evp.h>

// global variables for logging
FILE* log_file = NULL;
int verbose_logging = 0;

// initialize logging system
void init_logging(const char* log_filename) {
    const char* log_env = getenv("RUDP_LOG");
    if (log_env && strcmp(log_env, "1") == 0) {
        verbose_logging = 1;
        log_file = fopen(log_filename, "w");
        if (!log_file) {
            perror("failed to open log file");
            exit(1);
        }
    }
}

// log event with timestamp
void log_event(const char* format, ...) {
    if (!verbose_logging || !log_file) return;
    
    char time_buffer[30];
    struct timeval tv;
    time_t curtime;
    
    gettimeofday(&tv, NULL);
    curtime = tv.tv_sec;
    
    // format the time part
    strftime(time_buffer, 30, "%Y-%m-%d %H:%M:%S", localtime(&curtime));
    
    // add microseconds and print to log file
    fprintf(log_file, "[%s.%06ld] [LOG] ", time_buffer, tv.tv_usec);
    
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    
    fprintf(log_file, "\n");
    fflush(log_file);
}

// cleanup logging
void cleanup_logging(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

// create socket
int create_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        exit(1);
    }
    
    if (port > 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        
        if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind failed");
            exit(1);
        }
    }
    
    return sockfd;
}

// send packet
int send_packet(int sockfd, struct sockaddr_in* addr, struct sham_packet* packet, int data_len) {
    int total_len = sizeof(struct sham_header) + data_len;
    int bytes_sent = sendto(sockfd, packet, total_len, 0, 
                           (struct sockaddr*)addr, sizeof(*addr));
    if (bytes_sent < 0) {
        perror("sendto failed");
        return -1;
    }
    return bytes_sent;
}

// receive packet
int recv_packet(int sockfd, struct sockaddr_in* addr, struct sham_packet* packet) {
    socklen_t addr_len = sizeof(*addr);
    int bytes_recv = recvfrom(sockfd, packet, sizeof(*packet), 0,
                             (struct sockaddr*)addr, &addr_len);
    if (bytes_recv < 0) {
        perror("recvfrom failed");
        return -1;
    }
    return bytes_recv;
}

// simulate packet loss
int simulate_packet_loss(float loss_rate) {
    if (loss_rate <= 0.0) return 0;
    
    float random_val = (float)rand() / RAND_MAX;
    return random_val < loss_rate;
}

// generate initial sequence number
uint32_t generate_initial_seq(void) {
    return rand() % 1000000 + 1000; // random starting seq num
}

// check if packet should be lost
int is_packet_lost(float loss_rate) {
    if (loss_rate <= 0.0) return 0;
    return simulate_packet_loss(loss_rate);
}

void calculate_md5(const char* filename) {
    unsigned char buf[4096];
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    FILE* f = fopen(filename, "rb");
    if (!f) {
        perror("failed to open file for md5");
        return;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        perror("EVP_MD_CTX_new failed");
        fclose(f);
        return;
    }

    if (EVP_DigestInit_ex(ctx, EVP_md5(), NULL) != 1) {
        fprintf(stderr, "EVP_DigestInit_ex failed\n");
        EVP_MD_CTX_free(ctx);
        fclose(f);
        return;
    }

    while (1) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n > 0) {
            if (EVP_DigestUpdate(ctx, buf, n) != 1) {
                fprintf(stderr, "EVP_DigestUpdate failed\n");
                EVP_MD_CTX_free(ctx);
                fclose(f);
                return;
            }
        }
        if (n < sizeof(buf)) {
            if (feof(f)) break;
            if (ferror(f)) {
                perror("fread failed");
                EVP_MD_CTX_free(ctx);
                fclose(f);
                return;
            }
        }
    }

    if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        fprintf(stderr, "EVP_DigestFinal_ex failed\n");
        EVP_MD_CTX_free(ctx);
        fclose(f);
        return;
    }
    EVP_MD_CTX_free(ctx);
    fclose(f);

    // print lowercase hex MD5
    char hex[digest_len * 2 + 1];
    for (unsigned int i = 0; i < digest_len; i++) {
        sprintf(hex + i * 2, "%02x", digest[i]);
    }
    hex[digest_len * 2] = '\0';
    printf("MD5: %s\n", hex);
}
