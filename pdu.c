#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include "cpe464.h"
#include "gethostbyname.h"
#include "networks.h"
#include "safeUtil.h"

#define MAXPDUBUF 1407

#define seqNumLen 4
#define chkSumLen 2
#define flagLen 1

#define FNAME_BAD 7
#define FNAME_OK 9
#define DATA 16
#define END_OF_FILE 10
#define FILENAME_INIT 8


int createPDU(uint8_t *pduBuffer, uint32_t sequenceNumber, uint8_t flag, uint8_t *payload, int payloadLen) {
    uint32_t net_seq = htonl(sequenceNumber); // Convert sequence number to network order (using htonl for 32-bit)

    // Build pduBuffer
    memcpy(pduBuffer, &net_seq, seqNumLen); // Copy sequence number into buffer (Network Order)
    memset(pduBuffer + seqNumLen, 0, chkSumLen); // Place holder checksum value
    memcpy(pduBuffer + seqNumLen + chkSumLen, &flag, flagLen); // Copy flag into buffer
    memcpy(pduBuffer + seqNumLen + chkSumLen + flagLen, payload, payloadLen); // Copy payload into buffer

    int pduLength = seqNumLen + chkSumLen + flagLen + payloadLen; // Calculate pduLength
    // Run checksum on preliminary PDU
    uint16_t checksum = in_cksum((unsigned short *)pduBuffer, pduLength);

    // Copy checksum into PDU buffer
    memcpy(pduBuffer + seqNumLen, &checksum, chkSumLen);

    return pduLength;
}

void printPDU(uint8_t * PDU, int pduLength) {
    
    // Verify checksum
    if (in_cksum((unsigned short *)PDU, pduLength) != 0) {
        printf("Checksum is wrong\n");
        exit(1);
    }

    // Declare working buffers
    uint32_t netSequenceNum = 0;
    uint8_t flag = 0;
    

    // Decifer PDU
    memcpy(&netSequenceNum, PDU, seqNumLen); // Retrieve sequence number (4 bytes)
    uint32_t hostSequenceNum = ntohl(netSequenceNum); // Convert sequence number to host order

    memcpy(&flag, PDU + seqNumLen + chkSumLen, flagLen); // Retrieve flag number

    int payloadLen = pduLength - seqNumLen - chkSumLen - flagLen; // Calculate payload length
    uint8_t payload[payloadLen];
    memcpy(payload,  PDU + seqNumLen + chkSumLen + flagLen, payloadLen); // Retrieve payload
    
    // Print PDU
    printf("\nSequence Number: %d\n", hostSequenceNum);
    printf("Flag: %d\n", flag);
    printf("Payload: %s\n", payload);
    printf("Payload Length: %d\n\n", payloadLen);

}

void printInit(uint8_t * PDU, int pduLength) {
    
    // Verify checksum
    if (in_cksum((unsigned short *)PDU, pduLength) != 0) {
        printf("Checksum is wrong\n");
        exit(1);
    }

    // Declare working buffers
    uint32_t netSequenceNum = 0;
    uint8_t flag = 0;
    

    // Decifer PDU
    memcpy(&netSequenceNum, PDU, seqNumLen); // Retrieve sequence number (4 bytes)
    uint32_t hostSequenceNum = ntohl(netSequenceNum); // Convert sequence number to host order

    memcpy(&flag, PDU + seqNumLen + chkSumLen, flagLen); // Retrieve flag number
    int payloadLen = pduLength - seqNumLen - chkSumLen - flagLen; // Calculate payload length
    int filename_len = payloadLen - 8;

    uint32_t netBuff = 0;
    uint32_t netWindow = 0;
    uint8_t filename[filename_len];
    
    uint8_t payload[payloadLen];
    memcpy(payload, PDU+7, payloadLen);

    memcpy(&netBuff,  PDU + seqNumLen + chkSumLen + flagLen, 4); // Retrieve payload
    memcpy(&netWindow,  PDU + seqNumLen + chkSumLen + flagLen + 4, 4); // Retrieve payload
    memcpy(filename, PDU + seqNumLen + chkSumLen + flagLen + 8, filename_len);

    uint32_t bufferSize = ntohl(netBuff);
    uint32_t windowSize = ntohl(netWindow);
    filename[filename_len] = '\0';

    if (flag == FILENAME_INIT)
    {
        printf("\n===Initial Packet======================================================================\n");
        printf("Sequence Number: %d  ", hostSequenceNum);
        printf("Flag: %d  ", flag);
        printf("Buffer Size: %d  ", bufferSize);
        printf("Window Size: %d  ", windowSize);
        printf("Filename: %s\n", filename);
        printf("========================================================================================\n");

    }
    else if (flag == DATA)
    {
        printf("\nData Packet\n");
        printf(" - Sequence Number: %d  ", hostSequenceNum);
        printf("   Flag: %d\n", flag);
        printf(" * Data: %s\n", payload);
    }
    else if (flag == END_OF_FILE)
    {
        printf("\n===EOF PACKET==========================================================================\n");
        printf(" - Sequence Number: %d  ", hostSequenceNum);
        printf("   Flag: %d\n", flag);
        printf("========================================================================================\n");

    }
    
        
}

int send_buf(uint8_t *buf, int fileNameLen, struct Connection * server, uint8_t flag, uint32_t *clientSeqNum, uint8_t *packet) {
    int payloadLen = 4 + 4 + fileNameLen; // Calculate payload length
    int packetLen = 7 + payloadLen;
    
    createPDU(packet, *clientSeqNum, flag, buf, payloadLen);

    if (flag == FNAME_BAD || flag == FNAME_OK)
        printf("\nSending Filename Response\n");
    
    else
        printInit(packet, packetLen);
    
    
    int sendLen = safeSendto(server->sk_num, packet, packetLen, 0, (struct sockaddr *)&server->address, sizeof(server->address));
    // printInit(packet, packetLen);
    
    return sendLen;
    
}

int recv_buf(uint8_t *buf, int packetLen, int serverSocketNumber, struct Connection *client, uint8_t *flag, uint32_t *clientSeqNum) {
    struct sockaddr_storage clientAddr;
    int clientAddrLen = sizeof(clientAddr);
    int recvLen = safeRecvfrom(serverSocketNumber, buf, MAXPDUBUF, 0, (struct sockaddr *)&clientAddr, &clientAddrLen);

    // Store the client's address in the client structure
    memcpy(&client->address, &clientAddr, clientAddrLen);
    memcpy(clientSeqNum, buf, 4);
    memcpy(flag, buf + 6, 1);


    if (*flag == FNAME_OK) 
    {
        printf("\nFilename exist\n");
    }
    else if (*flag == FILENAME_INIT)
    { 
        printInit(buf, recvLen);
    }


    return recvLen;
}