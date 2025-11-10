#ifndef SHAM_H
#define SHAM_H

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <stdarg.h>
// #include <openssl/md5.h>  // commented out for now

// S.H.A.M. packet header structure
struct sham_header {
    uint32_t seq_num;      // sequence number
    uint32_t ack_num;      // acknowledgment number  
    uint16_t flags;        // control flags (SYN, ACK, FIN)
    uint16_t window_size;  // flow control window size
};

// flag definitions
#define SYN_FLAG 0x1
#define ACK_FLAG 0x2
#define FIN_FLAG 0x4

// protocol constants
#define MAX_DATA_SIZE 1024
#define WINDOW_SIZE 10
#define RTO_MS 500
#define BUFFER_SIZE 8192

// packet structure with header and data
struct sham_packet {
    struct sham_header header;
    char data[MAX_DATA_SIZE];
};

// connection state
typedef enum {
    CLOSED,
    SYN_SENT,
    SYN_RCVD,
    ESTABLISHED,
    FIN_WAIT_1,
    FIN_WAIT_2,
    CLOSE_WAIT,
    LAST_ACK,
    TIME_WAIT
} connection_state_t;

// packet info for tracking
struct packet_info {
    uint32_t seq_num;
    int data_len;
    long     file_offset;
    struct timeval sent_time;
    int retransmitted;
};




// global variables for logging
extern FILE* log_file;
extern int verbose_logging;

// function prototypes
void init_logging(const char* log_filename);
void log_event(const char* format, ...);
void cleanup_logging(void);

int create_socket(int port);
int send_packet(int sockfd, struct sockaddr_in* addr, struct sham_packet* packet, int data_len);
int recv_packet(int sockfd, struct sockaddr_in* addr, struct sham_packet* packet);
int simulate_packet_loss(float loss_rate);

// connection management
int three_way_handshake_client(int sockfd, struct sockaddr_in* server_addr, uint32_t* initial_seq);
int three_way_handshake_server(int sockfd, struct sockaddr_in* client_addr, uint32_t* initial_seq);
int four_way_handshake_close(int sockfd, struct sockaddr_in* addr, int is_initiator);

// data transmission
int send_file(int sockfd, struct sockaddr_in* addr, const char* filename, float loss_rate);
int receive_file(int sockfd, struct sockaddr_in* addr, const char* filename, float loss_rate);

// chat functionality
int chat_mode(int sockfd, struct sockaddr_in* addr, int is_server);

// utility functions
uint32_t generate_initial_seq(void);
void calculate_md5(const char* filename);
int is_packet_lost(float loss_rate);

#endif
 
