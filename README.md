# S.H.A.M. - Reliable UDP Protocol Implementation

A custom reliable UDP protocol implementation in C featuring connection management, packet loss simulation, and detailed event logging for educational purposes.

## Overview

S.H.A.M. (Simple, Handshake-based, Acknowledged Messaging) implements TCP-like reliability over UDP, including:
- Three-way handshake (SYN, SYN-ACK, ACK) for connection establishment
- Four-way connection termination (FIN, ACK, FIN, ACK)
- Sequence and acknowledgment numbers
- Configurable packet loss simulation for testing
- Comprehensive event logging

## Features

- **Reliable Transport**: Implements sequence numbers, acknowledgments, and retransmission
- **Connection Management**: Proper connection setup and teardown with state management
- **Flow Control**: Sliding window mechanism (default window size: 10 packets)
- **Packet Loss Simulation**: Configurable loss rate for protocol testing
- **Detailed Logging**: Timestamped event logs for debugging and analysis
- **File Transfer**: Support for reliable file transmission over UDP
- **Chat Mode**: Interactive chat session capability

## Project Structure
├── sham.h # Protocol header file (structs, constants, function prototypes)
├── server.c # Server implementation with handshake and data handling
├── Makefile # Build configuration
└── client_log.txt # Sample client log output

text

## Protocol Details

### Packet Structure
Each S.H.A.M. packet contains:
- **Sequence Number**: 32-bit unsigned integer
- **Acknowledgment Number**: 32-bit unsigned integer
- **Flags**: SYN (0x1), ACK (0x2), FIN (0x4)
- **Window Size**: 16-bit flow control window
- **Payload**: Up to 1024 bytes of data

### Connection States
- `CLOSED`: No connection
- `SYN_SENT`: SYN sent, awaiting SYN-ACK
- `SYN_RCVD`: SYN received, awaiting final ACK
- `ESTABLISHED`: Connection active
- `FIN_WAIT_1/2`: FIN sent, awaiting ACK and peer FIN
- `CLOSE_WAIT`: FIN received, awaiting close
- `LAST_ACK`: Final FIN sent
- `TIME_WAIT`: Connection closing grace period

## Compilation

Build the project using the provided Makefile:

make

text

This generates the `server` executable.

To clean build artifacts:

make clean

text

## Usage

### Start Server (File Transfer Mode)

./server <port> [loss_rate]

text

- `port`: UDP port number to listen on
- `loss_rate` (optional): Packet loss probability (0.0 to 1.0, e.g., 0.1 for 10% loss)

**Example:**
./server 8080 0.1

text

### Start Server (Chat Mode)

./server <port> --chat

text

**Example:**
./server 9000 --chat

text

## Logging

All protocol events are logged with microsecond timestamps in the format:

YYYY-MM-DD HH:MM:SS.microseconds LOG <EVENT> <DETAILS>

text

### Sample Log Output

2025-10-15 23:28:41.136099 LOG SND SYN SEQ290383
2025-10-15 23:28:41.136322 LOG RCV SYN-ACK SEQ290383 ACK290384
2025-10-15 23:28:41.136350 LOG SND ACK FOR SYN
2025-10-15 23:28:41.136376 LOG SND DATA SEQ290384 LEN8
2025-10-15 23:28:41.136479 LOG RCV ACK290392
2025-10-15 23:28:41.136504 LOG SND FIN SEQ290392

text

Logs are written to:
- `server_log.txt` (server-side events)
- `client_log.txt` (client-side events)

## Technical Specifications

- **Max Data Size**: 1024 bytes per packet
- **Buffer Size**: 8192 bytes
- **Default Window**: 10 packets
- **RTO (Retransmission Timeout)**: 500 ms
- **Transport**: UDP (with reliability layer)

## Dependencies

- Standard C library
- POSIX sockets (`sys/socket.h`, `netinet/in.h`, `arpa/inet.h`)
- OpenSSL (MD5 - currently disabled in code)

## Requirements

- Linux/Unix-like operating system
- GCC compiler
- Make utility

## Future Enhancements

- Implement congestion control (e.g., AIMD)
- Add selective acknowledgment (SACK)
- Support for out-of-order packet handling
- Encrypted payload using OpenSSL
- Client implementation with interactive CLI

## Acknowledgements

This project was developed as part of the **Operating Systems and Networks** course at the **International Institute of Information Technology, Hyderabad (IIIT-H)**. Special thanks to the course instructor and teaching assistants for their guidance and support throughout the development of this shell implementation.
