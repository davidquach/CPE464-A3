#ifndef PDU_H
#define PDU_H

#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include "cpe464.h"
#include "gethostbyname.h"
#include "networks.h"
#include "safeUtil.h"


#define seqNumLen 4
#define chkSumLen 2
#define flagLen 1

#define FNAME_BAD 7
#define FNAME_OK 9
#define DATA 16
#define END_OF_FILE 10
#define FILENAME_INIT 8
#define RR 5
#define EOF_ACK 32
#define DATA_TIMEOUT 18
#define SREJ 6
#define SREJ_RETRAN 17



int createPDU(uint8_t *pduBuffer, uint32_t sequenceNumber, uint8_t flag, uint8_t *payload, int payloadLen);
void printPDU(uint8_t * PDU, int pduLength);
void printPacket(uint8_t * PDU, int pduLength);
int send_init(uint8_t *buf, int dataLen, struct Connection * server, uint8_t flag, uint32_t *clientSeqNum, uint8_t *packet);
int recv_buf(uint8_t *buf, int packetLen, int serverSocketNumber, struct Connection * client, uint8_t *flag, uint32_t *clientSeqNum);
int send_buf(uint8_t *data, int dataLen, struct Connection * server, uint8_t flag, uint32_t *clientSeqNum, uint8_t *packet);

#endif