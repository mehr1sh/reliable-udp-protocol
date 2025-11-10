#include "sham.h"

// server state
static connection_state_t state = CLOSED;
static uint32_t server_seq = 0;
static uint32_t client_seq = 0;
static int sockfd = -1;
// static struct sockaddr_in client_addr;  // not used
static char received_filename[256] = {0};

// three way handshake for server
int three_way_handshake_server(int sockfd, struct sockaddr_in *client_addr, uint32_t *initial_seq)
{
    struct sham_packet packet;
    socklen_t addr_len = sizeof(*client_addr);

    // step 1: receive SYN from client
    int bytes_recv = recvfrom(sockfd, &packet, sizeof(packet), 0,
                              (struct sockaddr *)client_addr, &addr_len);
    if (bytes_recv < 0)
    {
        perror("recvfrom failed in handshake");
        return -1;
    }

    if (!(packet.header.flags & SYN_FLAG))
    {
        fprintf(stderr, "expected SYN packet\n");
        return -1;
    }

    client_seq = packet.header.seq_num;
    log_event("RCV SYN SEQ=%u", client_seq);

    // step 2: send SYN-ACK
    server_seq = generate_initial_seq();
    packet.header.seq_num = server_seq;
    packet.header.ack_num = client_seq + 1;
    packet.header.flags = SYN_FLAG | ACK_FLAG;
    packet.header.window_size = BUFFER_SIZE;

    if (send_packet(sockfd, client_addr, &packet, 0) < 0)
    {
        return -1;
    }

    log_event("SND SYN-ACK SEQ=%u ACK=%u", server_seq, client_seq + 1);
    state = SYN_RCVD;

    // step 3: receive ACK from client
    bytes_recv = recvfrom(sockfd, &packet, sizeof(packet), 0,
                          (struct sockaddr *)client_addr, &addr_len);
    if (bytes_recv < 0)
    {
        perror("recvfrom failed in handshake step 3");
        return -1;
    }

    if (!(packet.header.flags & ACK_FLAG) || packet.header.ack_num != server_seq + 1)
    {
        fprintf(stderr, "invalid ACK in handshake\n");
        return -1;
    }

    log_event("RCV ACK FOR SYN");
    state = ESTABLISHED;
    *initial_seq = server_seq;

    return 0;
}

// receive file
int receive_file(int sockfd, struct sockaddr_in *addr, const char *filename, float loss_rate)
{
    FILE *file = fopen(filename, "wb");
    if (!file)
    {
        perror("failed to create output file");
        return -1;
    }

    uint32_t expected_seq = client_seq + 1;
    struct sham_packet ack_packet;
    ack_packet.header.flags = ACK_FLAG;
    ack_packet.header.window_size = BUFFER_SIZE;

    while (1)
    {
        struct sham_packet packet;
        socklen_t addr_len = sizeof(*addr);
        int bytes_recv = recvfrom(sockfd, &packet, sizeof(packet), 0,
                                  (struct sockaddr *)addr, &addr_len);
        if (bytes_recv < 0)
        {
            perror("recvfrom failed");
            break;
        }

        if (packet.header.flags & FIN_FLAG)
        {
            log_event("RCV FIN SEQ=%u", packet.header.seq_num);
            uint32_t peer_fin_seq = packet.header.seq_num;

            // ACK their FIN
            ack_packet.header.seq_num = server_seq; // FIX
            ack_packet.header.ack_num = peer_fin_seq + 1;
            ack_packet.header.flags = ACK_FLAG;
            send_packet(sockfd, addr, &ack_packet, 0);
            log_event("SND ACK FOR FIN");

            // send our FIN
            ack_packet.header.seq_num = server_seq;
            ack_packet.header.flags = FIN_FLAG;
            send_packet(sockfd, addr, &ack_packet, 0);
            log_event("SND FIN SEQ=%u", server_seq);

            // wait final ACK
            bytes_recv = recvfrom(sockfd, &packet, sizeof(packet), 0,
                                  (struct sockaddr *)addr, &addr_len);
            if (bytes_recv > 0 && (packet.header.flags & ACK_FLAG))
            {
                log_event("RCV ACK=%u", packet.header.ack_num);
            }
            break;
        }

        if (is_packet_lost(loss_rate))
        {
            log_event("DROP DATA SEQ=%u", packet.header.seq_num);
            continue;
        }

        int data_len = bytes_recv - sizeof(struct sham_header);
        log_event("RCV DATA SEQ=%u LEN=%d", packet.header.seq_num, data_len);

        if (packet.header.seq_num == expected_seq)
        {
            fwrite(packet.data, 1, data_len, file);
            expected_seq += data_len;
        }

        // always ACK the next expected byte
        ack_packet.header.seq_num = server_seq; // FIX
        ack_packet.header.ack_num = expected_seq;
        ack_packet.header.flags = ACK_FLAG;
        ack_packet.header.window_size = BUFFER_SIZE;
        send_packet(sockfd, addr, &ack_packet, 0);
        log_event("SND ACK=%u WIN=%u", ack_packet.header.ack_num, ack_packet.header.window_size);
    }

    fclose(file);
    return 0;
}

// chat mode for server
int chat_mode(int sockfd, struct sockaddr_in *addr, int is_server)
{
    (void)is_server; // suppress unused parameter warning
    fd_set readfds;
    char input_buffer[1024];
    struct sham_packet packet;

    printf("chat mode started. type /quit to exit\n");

    while (1)
    {
        FD_ZERO(&readfds);
        FD_SET(0, &readfds); // stdin
        FD_SET(sockfd, &readfds);

        int max_fd = (sockfd > 0) ? sockfd : 0;

        if (select(max_fd + 1, &readfds, NULL, NULL, NULL) < 0)
        {
            perror("select failed");
            break;
        }

        // check for input from stdin
        if (FD_ISSET(0, &readfds))
        {
            if (fgets(input_buffer, sizeof(input_buffer), stdin))
            {
                // strip newline
                int msg_len = strlen(input_buffer);
                if (msg_len > 0 && input_buffer[msg_len - 1] == '\n')
                {
                    input_buffer[msg_len - 1] = '\0';
                    msg_len--;
                }

                if (strcmp(input_buffer, "/quit") == 0)
                {
                    // initiate 4-way handshake close
                    four_way_handshake_close(sockfd, addr, 1);
                    break;
                }

                // send message
                packet.header.seq_num = server_seq;
                packet.header.ack_num = 0;
                packet.header.flags = 0;
                packet.header.window_size = BUFFER_SIZE;

                if (msg_len > 0 && msg_len < MAX_DATA_SIZE)
                {
                    memcpy(packet.data, input_buffer, msg_len);
                    packet.data[msg_len] = '\0';
                }

                send_packet(sockfd, addr, &packet, msg_len);
                log_event("SND DATA SEQ=%u LEN=%d", server_seq, msg_len);
                server_seq += msg_len;
            }
        }

        // check for data from network
        if (FD_ISSET(sockfd, &readfds))
        {
            socklen_t addr_len = sizeof(*addr);
            int bytes_recv = recvfrom(sockfd, &packet, sizeof(packet), 0,
                                      (struct sockaddr *)addr, &addr_len);
            if (bytes_recv > 0)
            {
                if (packet.header.flags & FIN_FLAG)
                {
                    // handle connection close
                    four_way_handshake_close(sockfd, addr, 0);
                    break;
                }
                else
                {
                    int data_len = bytes_recv - sizeof(struct sham_header);
                    if (data_len > 0)
                    {
                        if (data_len >= MAX_DATA_SIZE)
                            data_len = MAX_DATA_SIZE - 1;
                        packet.data[data_len] = '\0';

                        // detect peer quit
                        if (strcmp(packet.data, "/quit") == 0)
                        {
                            printf("peer disconnected\n");
                            break;
                        }

                        printf("received: %s\n", packet.data);
                    }

                    // send ACK
                    struct sham_packet ack_packet;
                    ack_packet.header.seq_num = server_seq;
                    ack_packet.header.ack_num = packet.header.seq_num + data_len;
                    ack_packet.header.flags = ACK_FLAG;
                    ack_packet.header.window_size = BUFFER_SIZE;

                    send_packet(sockfd, addr, &ack_packet, 0);
                    log_event("SND ACK=%u", ack_packet.header.ack_num);
                }
            }
        }
    }

    return 0;
}

// 4-way handshake for connection close (safe + timeout)
int four_way_handshake_close(int sockfd, struct sockaddr_in *addr, int is_initiator)
{
    struct sham_packet packet;
    socklen_t addr_len = sizeof(*addr);
    struct timeval tv;

    // set 1s timeout so we don’t hang forever
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (is_initiator)
    {
        // step 1: send FIN
        packet.header.seq_num = server_seq;
        packet.header.ack_num = 0;
        packet.header.flags = FIN_FLAG;
        packet.header.window_size = BUFFER_SIZE;

        send_packet(sockfd, addr, &packet, 0);
        log_event("SND FIN SEQ=%u", server_seq);
        state = FIN_WAIT_1;

        // step 2: wait for ACK
        int bytes_recv = recvfrom(sockfd, &packet, sizeof(packet), 0,
                                  (struct sockaddr *)addr, &addr_len);
        if (bytes_recv > 0 && (packet.header.flags & ACK_FLAG))
        {
            log_event("RCV ACK FOR FIN");
            state = FIN_WAIT_2;
        }

        // step 3: wait for peer’s FIN
        bytes_recv = recvfrom(sockfd, &packet, sizeof(packet), 0,
                              (struct sockaddr *)addr, &addr_len);
        if (bytes_recv > 0 && (packet.header.flags & FIN_FLAG))
        {
            uint32_t peer_fin_seq = packet.header.seq_num;
            log_event("RCV FIN SEQ=%u", peer_fin_seq);

            // step 4: send final ACK
            struct sham_packet ack_packet;
            ack_packet.header.seq_num = server_seq;
            ack_packet.header.ack_num = peer_fin_seq + 1;
            ack_packet.header.flags = ACK_FLAG;
            ack_packet.header.window_size = BUFFER_SIZE;

            send_packet(sockfd, addr, &ack_packet, 0);
            log_event("SND ACK=%u", ack_packet.header.ack_num);
            state = TIME_WAIT;
        }
    }
    else
    {
        // passive side: wait for FIN
        int bytes_recv = recvfrom(sockfd, &packet, sizeof(packet), 0,
                                  (struct sockaddr *)addr, &addr_len);
        if (bytes_recv > 0 && (packet.header.flags & FIN_FLAG))
        {
            uint32_t peer_fin_seq = packet.header.seq_num;
            log_event("RCV FIN SEQ=%u", peer_fin_seq);

            // send ACK
            struct sham_packet ack_packet;
            ack_packet.header.seq_num = server_seq;
            ack_packet.header.ack_num = peer_fin_seq + 1;
            ack_packet.header.flags = ACK_FLAG;
            ack_packet.header.window_size = BUFFER_SIZE;

            send_packet(sockfd, addr, &ack_packet, 0);
            log_event("SND ACK FOR FIN");
            state = CLOSE_WAIT;

            // send our own FIN
            packet.header.seq_num = server_seq;
            packet.header.ack_num = 0;
            packet.header.flags = FIN_FLAG;
            packet.header.window_size = BUFFER_SIZE;

            send_packet(sockfd, addr, &packet, 0);
            log_event("SND FIN SEQ=%u", server_seq);
            state = LAST_ACK;

            // wait for final ACK (with timeout)
            bytes_recv = recvfrom(sockfd, &packet, sizeof(packet), 0,
                                  (struct sockaddr *)addr, &addr_len);
            if (bytes_recv > 0 && (packet.header.flags & ACK_FLAG))
            {
                log_event("RCV ACK=%u", packet.header.ack_num);
                state = CLOSED;
            }
        }
    }

    // restore blocking mode (remove timeout)
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <port> [--chat] [loss_rate]\n", argv[0]);
        exit(1);
    }

    int port = atoi(argv[1]);
    int chat_mode_flag = 0;
    float loss_rate = 0.0;

    // parse arguments
    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "--chat") == 0)
        {
            chat_mode_flag = 1;
        }
        else
        {
            loss_rate = atof(argv[i]);
        }
    }

    // initialize logging
    init_logging("server_log.txt");

    // create socket
    sockfd = create_socket(port);
    fprintf(stderr, "server listening on port %d\n", port);
    // wait for client connection
    struct sockaddr_in client_addr;
    uint32_t initial_seq;

    if (three_way_handshake_server(sockfd, &client_addr, &initial_seq) < 0)
    {
        fprintf(stderr, "handshake failed\n");
        cleanup_logging();
        close(sockfd);
        exit(1);
    }

    fprintf(stderr, "connection established\n");

    if (chat_mode_flag)
    {
        chat_mode(sockfd, &client_addr, 1);
    }
    else
    {
        // file transfer mode
        strcpy(received_filename, "received_file");
        if (receive_file(sockfd, &client_addr, received_filename, loss_rate) < 0)
        {
            fprintf(stderr, "file transfer failed\n");
        }
        else
        {
            fprintf(stderr, "file received successfully\n");
            calculate_md5(received_filename); 
        }
    }

    cleanup_logging();
    close(sockfd);
    return 0;
}
