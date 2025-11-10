#include "sham.h"

// client state
static connection_state_t state = CLOSED;
static uint32_t client_seq = 0;
static uint32_t server_seq = 0;
static int sockfd = -1;
static struct sockaddr_in server_addr;

// three way handshake for client
int three_way_handshake_client(int sockfd, struct sockaddr_in *server_addr, uint32_t *initial_seq)
{
    struct sham_packet packet;

    // step 1: send SYN
    client_seq = generate_initial_seq();
    packet.header.seq_num = client_seq;
    packet.header.ack_num = 0;
    packet.header.flags = SYN_FLAG;
    packet.header.window_size = BUFFER_SIZE;

    if (send_packet(sockfd, server_addr, &packet, 0) < 0)
    {
        return -1;
    }

    log_event("SND SYN SEQ=%u", client_seq);
    state = SYN_SENT;

    // step 2: receive SYN-ACK with timeout
    socklen_t addr_len = sizeof(*server_addr);
    fd_set readfds;
    struct timeval tv;
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    tv.tv_sec = 10;  // 10 second timeout
    tv.tv_usec = 0;

    int sel = select(sockfd + 1, &readfds, NULL, NULL, &tv);
    if (sel <= 0) {
        if (sel == 0) {
            fprintf(stderr, "connection timeout: server not responding\n");
        } else {
            perror("select failed during handshake");
        }
        return -1;
    }

    int bytes_recv = recvfrom(sockfd, &packet, sizeof(packet), 0,
                             (struct sockaddr *)server_addr, &addr_len);
    if (bytes_recv < 0)
    {
        perror("recvfrom failed in handshake");
        return -1;
    }

    if (!(packet.header.flags & (SYN_FLAG | ACK_FLAG)))
    {
        fprintf(stderr, "expected SYN-ACK packet\n");
        return -1;
    }

    server_seq = packet.header.seq_num;
    log_event("RCV SYN-ACK SEQ=%u ACK=%u", server_seq, packet.header.ack_num);

    if (packet.header.ack_num != client_seq + 1)
    {
        fprintf(stderr, "invalid ACK number in SYN-ACK\n");
        return -1;
    }

    // step 3: send ACK
    packet.header.seq_num = client_seq;
    packet.header.ack_num = server_seq + 1;
    packet.header.flags = ACK_FLAG;
    packet.header.window_size = BUFFER_SIZE;

    if (send_packet(sockfd, server_addr, &packet, 0) < 0)
    {
        return -1;
    }

    log_event("SND ACK FOR SYN");
    state = ESTABLISHED;
    client_seq++; // increment for data packets
    *initial_seq = client_seq;
    return 0;
}

// send file with sliding window and retransmission
int send_file(int sockfd, struct sockaddr_in *addr, const char *filename, float loss_rate)
{
    (void)loss_rate;
    
    FILE *file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "failed to open input file '%s': %s\n", 
                filename, strerror(errno));
        return -1;
    }

    // Verify file is readable
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0) {
        fprintf(stderr, "input file '%s' is empty or unreadable\n", filename);
        fclose(file);
        return -1;
    }

    struct sham_packet packet;
    struct packet_info window[WINDOW_SIZE];
    int window_start = 0, window_end = 0;
    long file_pos = 0; // track absolute file offset
    int bytes_read = 0;
    char buffer[MAX_DATA_SIZE];

    packet.header.flags = 0;
    packet.header.window_size = BUFFER_SIZE;

    while (1)
    {
        // fill window
        while (window_end - window_start < WINDOW_SIZE)
        {
            if (fseek(file, file_pos, SEEK_SET) != 0)
                break;

            bytes_read = fread(buffer, 1, MAX_DATA_SIZE, file);
            if (bytes_read <= 0)
                break;

            // setup packet
            packet.header.seq_num = client_seq; // use current seq
            packet.header.ack_num = 0;
            memcpy(packet.data, buffer, bytes_read);

            if (send_packet(sockfd, addr, &packet, bytes_read) < 0)
            {
                fclose(file);
                return -1;
            }

            log_event("SND DATA SEQ=%u LEN=%d", packet.header.seq_num, bytes_read);

            // track packet
            int idx = window_end % WINDOW_SIZE;
            window[idx].seq_num = packet.header.seq_num;
            window[idx].data_len = bytes_read;
            window[idx].file_offset = file_pos; // save offset
            gettimeofday(&window[idx].sent_time, NULL);
            window[idx].retransmitted = 0;

            client_seq += bytes_read; // advance once per packet
            file_pos += bytes_read;
            window_end++;
        }

        // check ACKs or timeout
        fd_set readfds;
        struct timeval timeout;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;

        int sel = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
        if (sel > 0 && FD_ISSET(sockfd, &readfds))
        {
            socklen_t addr_len = sizeof(*addr);
            int rcv = recvfrom(sockfd, &packet, sizeof(packet), 0,
                              (struct sockaddr *)addr, &addr_len);

            if (rcv > 0 && (packet.header.flags & ACK_FLAG))
            {
                log_event("RCV ACK=%u", packet.header.ack_num);
                uint32_t ack_num = packet.header.ack_num;

                while (window_start < window_end)
                {
                    int idx = window_start % WINDOW_SIZE;
                    if (window[idx].seq_num + window[idx].data_len <= ack_num)
                    {
                        window_start++;
                    }
                    else
                        break;
                }
            }
        }
        else if (sel == 0)
        {
            // timeout - retransmit
            struct timeval now;
            gettimeofday(&now, NULL);

            for (int i = window_start; i < window_end; i++)
            {
                int idx = i % WINDOW_SIZE;
                long elapsed_ms = (now.tv_sec - window[idx].sent_time.tv_sec) * 1000 +
                                 (now.tv_usec - window[idx].sent_time.tv_usec) / 1000;

                if (elapsed_ms > RTO_MS)
                {
                    log_event("TIMEOUT SEQ=%u", window[idx].seq_num);
                    packet.header.seq_num = window[idx].seq_num;
                    packet.header.ack_num = 0;

                    // reread original data
                    fseek(file, window[idx].file_offset, SEEK_SET);
                    int retx_bytes = fread(buffer, 1, window[idx].data_len, file);
                    memcpy(packet.data, buffer, retx_bytes);

                    send_packet(sockfd, addr, &packet, retx_bytes);
                    log_event("RETX DATA SEQ=%u LEN=%d", window[idx].seq_num, retx_bytes);

                    gettimeofday(&window[idx].sent_time, NULL);
                    window[idx].retransmitted = 1;
                }
            }
        }

        if (window_start == window_end && bytes_read <= 0)
            break;
    }

    fclose(file);

    // send FIN
    packet.header.seq_num = client_seq; // next unsent seq
    packet.header.ack_num = 0;
    packet.header.flags = FIN_FLAG;
    packet.header.window_size = BUFFER_SIZE;
    send_packet(sockfd, addr, &packet, 0);
    log_event("SND FIN SEQ=%u", packet.header.seq_num);

    // normal FIN/ACK handshake unchanged
    socklen_t addr_len = sizeof(*addr);
    int rcv = recvfrom(sockfd, &packet, sizeof(packet), 0,
                      (struct sockaddr *)addr, &addr_len);

    if (rcv > 0 && (packet.header.flags & ACK_FLAG))
    {
        log_event("RCV ACK FOR FIN");
    }

    rcv = recvfrom(sockfd, &packet, sizeof(packet), 0,
                  (struct sockaddr *)addr, &addr_len);

    if (rcv > 0 && (packet.header.flags & FIN_FLAG))
    {
        log_event("RCV FIN SEQ=%u", packet.header.seq_num);
        packet.header.seq_num = client_seq;
        packet.header.ack_num = packet.header.seq_num + 1;
        packet.header.flags = ACK_FLAG;
        packet.header.window_size = BUFFER_SIZE;
        send_packet(sockfd, addr, &packet, 0);
        log_event("SND ACK=%u", packet.header.ack_num);
    }

    return 0;
}

// chat mode for client
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
                packet.header.seq_num = client_seq;
                packet.header.ack_num = 0;
                packet.header.flags = 0;
                packet.header.window_size = BUFFER_SIZE;

                if (msg_len > 0 && msg_len < MAX_DATA_SIZE)
                {
                    memcpy(packet.data, input_buffer, msg_len);
                    packet.data[msg_len] = '\0';
                }

                send_packet(sockfd, addr, &packet, msg_len);
                log_event("SND DATA SEQ=%u LEN=%d", client_seq, msg_len);
                client_seq += msg_len;
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

                        // check if peer sent /quit
                        if (strcmp(packet.data, "/quit") == 0)
                        {
                            printf("peer disconnected\n");
                            break;
                        }

                        printf("received: %s\n", packet.data);
                    }

                    // send ACK
                    struct sham_packet ack_packet;
                    ack_packet.header.seq_num = client_seq;
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

    // set 1s timeout so we don't hang forever
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

        // step 3: wait for peer's FIN
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
    if (argc < 4)
    {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  File mode: %s <server_ip> <server_port> <input_file> <output_file> [loss_rate]\n", argv[0]);
        fprintf(stderr, "  Chat mode: %s <server_ip> <server_port> --chat [loss_rate]\n", argv[0]);
        fprintf(stderr, "\nExamples:\n");
        fprintf(stderr, "  %s 127.0.0.1 8080 input.txt output.txt\n", argv[0]);
        fprintf(stderr, "  %s 127.0.0.1 8080 input.txt output.txt 0.1\n", argv[0]);
        fprintf(stderr, "  %s 127.0.0.1 8080 --chat\n", argv[0]);
        fprintf(stderr, "  %s 127.0.0.1 8080 --chat 0.1\n", argv[0]);
        exit(1);
    }

    char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    int chat_mode_flag = 0;
    float loss_rate = 0.0;
    char *input_file = NULL;

    // parse arguments
    if (strcmp(argv[3], "--chat") == 0)
    {
        chat_mode_flag = 1;
        if (argc > 4)
        {
            loss_rate = atof(argv[4]);
        }
    }
    else
    {
        input_file = argv[3];
        if (argc > 5)
        {
            loss_rate = atof(argv[5]);
        }
    }

    // Validate input file exists (add this after argument parsing, before socket creation)
    if (!chat_mode_flag && input_file) {
        // Check if input file exists and is readable
        FILE *test_file = fopen(input_file, "rb");
        if (!test_file) {
            fprintf(stderr, "Error: Cannot open input file '%s': %s\n", 
                    input_file, strerror(errno));
            exit(1);
        }
        
        // Check if file is empty
        fseek(test_file, 0, SEEK_END);
        long file_size = ftell(test_file);
        fclose(test_file);
        
        if (file_size <= 0) {
            fprintf(stderr, "Error: Input file '%s' is empty\n", input_file);
            exit(1);
        }
        
        printf("Input file '%s' validated (%ld bytes)\n", input_file, file_size);
    }

    // initialize logging
    init_logging("client_log.txt");

    // create socket
    sockfd = create_socket(0);

    // Set connection timeout
    struct timeval timeout;
    timeout.tv_sec = 10;  // 10 second timeout
    timeout.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("failed to set socket timeout");
        cleanup_logging();
        close(sockfd);
        exit(1);
    }

    // setup server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0)
    {
        fprintf(stderr, "Error: Invalid server IP address '%s'\n", server_ip);
        fprintf(stderr, "Please use a valid IPv4 address (e.g., 127.0.0.1)\n");
        cleanup_logging();
        close(sockfd);
        exit(1);
    }

    // perform handshake
    uint32_t initial_seq;
    if (three_way_handshake_client(sockfd, &server_addr, &initial_seq) < 0)
    {
        fprintf(stderr, "handshake failed\n");
        cleanup_logging();
        close(sockfd);
        exit(1);
    }

    printf("connection established\n");

    if (chat_mode_flag)
    {
        chat_mode(sockfd, &server_addr, 0);
    }
    else
    {
        // file transfer mode
        if (send_file(sockfd, &server_addr, input_file, loss_rate) < 0)
        {
            fprintf(stderr, "file transfer failed\n");
        }
        else
        {
            printf("file sent successfully\n");
        }
    }

    cleanup_logging();
    close(sockfd);
    return 0;
}