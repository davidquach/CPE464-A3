// Client side - UDP Code				    
// By Hugh Smith	4/1/2017		

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include "gethostbyname.h"
#include "networks.h"
#include "safeUtil.h"
#include "pdu.h"
#include "pollLib.h"
#include "window.h"

#define MAXBUF 1400
#define MAXPDUBUF 1407
#define MAXFILELEN 100
#define MAXWINDOW 1073741824
#define MAX_RETRANS 10
#define START_SEQ_NUM 1


typedef enum State STATE;

enum State
{
	START_STATE, DONE, FILENAME, WAIT_FILE_ACK, FILE_OK, RECV_DATA, BUFFER, FLUSH
};

void talkToServer(int socketNum, struct sockaddr_in6 * server);
int readFromStdin(char * buffer);
void checkArgs(int argc, char * argv[]);
void processFile (char * argv[]);
STATE filename (char * fname, int32_t buf_size, struct Connection * server);
STATE processSelect(struct Connection *connection, int *retryCount, STATE TimeoutState, STATE DataState, STATE DoneState);
STATE file_ok(int * outputFileFd, char *outputFileName, struct window *clientWindow, int32_t window_size);
STATE recv_data(int32_t output_file, struct Connection * server, uint32_t * clientSeqNum, struct window *clientWindow, uint32_t *expected,  uint32_t *highest);
STATE buffer(int32_t output_file, struct Connection * server, uint32_t * clientSeqNum, struct window *clientWindow, uint32_t *expected, uint32_t *highest);
STATE flush(int32_t output_file, struct Connection * server, uint32_t * clientSeqNum, struct window *clientWindow, uint32_t *expected, uint32_t *highest);
void writeDisk(int outputFileFd, uint32_t packet_len, uint8_t *packet, struct window *clientWindow, uint32_t seq_num);

uint32_t data_packet_len = 0;
uint32_t final_packet_len = 0;
uint32_t final_packet_seq = 0;
uint32_t eof_seq = 0;

STATE start_state(char ** argv, struct Connection * server, uint32_t * clientSeqNum) 
{
	uint8_t packet[MAXPDUBUF]; // Includes PDU header and data payload (1407)
	uint8_t buf[MAXBUF]; // Includes data payload (1400)
	STATE returnValue = DONE;

	// Packet variables 
	uint32_t bufferSize = 0;
	uint32_t windowSize = 0;
	int fileNameLen = 0;
	uint8_t flag = FILENAME_INIT; // Packet contains the file name/buffer-size/window-size (rcopy to server)
	
	// Condition to check if server has been connected before
	if (server->sk_num > 0) 
	{
		close(server->sk_num);
	}

	// Setup UDP 
	int portNumber = atoi(argv[7]);
	int socketNum = setupUdpClientToServer(&server->address, argv[6], portNumber); 

	// Could not connet to to server
	if (socketNum < 0) 
	{
		returnValue = DONE;
	}

	else 
	{
        
		// Setup poll table
		setupPollSet();
		addToPollSet(socketNum);
		server->sk_num = socketNum; // Set socket number

		// Retrieve establishment variables
		bufferSize = htonl(atoi(argv[4])); // Convert buffer size to network order
		data_packet_len = 7 + atoi(argv[4]);

		windowSize = htonl(atoi(argv[3])); // Convert window size to network order
		fileNameLen = strlen(argv[1]);

		// Build buffer
		memcpy(buf, &bufferSize, 4);
		memcpy(buf + 4, &windowSize, 4);
		memcpy(buf + 8, argv[1], fileNameLen);
		
		send_init(buf, fileNameLen, server, flag, clientSeqNum, packet);
		
        (*clientSeqNum)++; // Increment sequence number

		returnValue = FILENAME; 
	}

	return returnValue;
}



int main (int argc, char *argv[])
 {

	checkArgs(argc, argv);	

	sendErr_init(atof(argv[5]), DROP_ON, FLIP_ON, DEBUG_ON, RSEED_ON); // Set error rate
		
	processFile(argv);
	
	return 0;
}


void processFile (char * argv[]) {
	struct Connection *server = (struct Connection *) calloc(1, sizeof(struct Connection));
	uint32_t clientSeqNum = 0;
	int32_t output_file_fd = 0;
	STATE state = START_STATE; // Start State
	struct window *clientWindow = (struct window *) calloc(1, sizeof(struct window));
	uint32_t expected = 1;
	uint32_t highest = 1;

	while (state != DONE) 
	{
		switch (state)
		{

			// START: establish connection with server and transmit filename, buffer size, and window size
			case START_STATE: 
				state = start_state(argv, server, &clientSeqNum);
				break;
				
			case FILENAME:
				state = filename(argv[1], atoi(argv[4]), server);
				break;
		
			case DONE:
				exit(0);
				break;
			
			case FILE_OK:
				state = file_ok(&output_file_fd, argv[2], clientWindow, atoi(argv[3]));
				break;
			
			case RECV_DATA:
				state = recv_data(output_file_fd, server, &clientSeqNum, clientWindow, &expected, &highest);
				break;

			case BUFFER:
				state = buffer(output_file_fd, server, &clientSeqNum, clientWindow, &expected, &highest);
				break;

			case FLUSH:
				state = flush(output_file_fd, server,  &clientSeqNum, clientWindow, &expected, &highest);
				break;

			case WAIT_FILE_ACK:
				break;
				
		}	
	}
}

STATE recv_data(int32_t output_file, struct Connection * server, uint32_t * clientSeqNum, struct window *clientWindow, uint32_t *expected, uint32_t *highest)
{
	
	uint32_t seq_num = 0;
	uint32_t ackSeqNum = 0;
	uint8_t flag = 0 ;
	int32_t data_len = 0;
	uint8_t data_buf[MAXPDUBUF];
	uint8_t packet[MAXPDUBUF];


	// Poll for 10 seconds
	if (pollCall(10000) == -1) {
		printf("Timed out waiting for data\n");
		return DONE;
	}

	// Receive Data Packet from Server
	data_len = recv_buf(data_buf, MAXPDUBUF, server->sk_num, server, &flag, &seq_num);
	
	// Check for Flipped bits
	if ((in_cksum((unsigned short *)data_buf, data_len) != 0) || (data_len == CRC_ERROR)) 
	{
		return RECV_DATA; // Ignore incorrect packet and continue waiting for initial packet.
	}

	// Populate Global Variables
	if (flag == DATA)
	{
		if (data_len != data_packet_len)
		{
			final_packet_len = data_len;
			final_packet_seq = seq_num;
			// printf("Final Packet Length: %d (Seq: %d)\n", final_packet_len, seq_num);
			// printf("Buffer: %s\n", data_buf);
			// printf("Final_packet_seq: %d\n", final_packet_seq);
		}
		else 
		{
			data_packet_len = data_len;
		}
	}
	else if (flag == END_OF_FILE) {
		eof_seq = seq_num;
	}


	// In Order Data
	if (seq_num == *expected)	
	{	
		// Send RR
		ackSeqNum = htonl(seq_num + 1); // RR value will be +1 the sequence number
		
		// Received EOF in order (Nothing to buffer and EXIT)
		if (flag == END_OF_FILE)
		{
			send_buf((uint8_t *)&ackSeqNum, sizeof(ackSeqNum), server, EOF_ACK, clientSeqNum, packet);
			printf("Finished Tranmission\n");
			exit(0);
		}
		
		// Received Data in order
		else
		{
			// Send RR for Data
			send_buf((uint8_t *)&ackSeqNum, sizeof(ackSeqNum), server, RR, clientSeqNum, packet);
		}


		//  Write in-order data to disk
		int actual_data_len = data_len - 7;
		uint8_t data[actual_data_len]; 
		memcpy(data, data_buf + 7, actual_data_len);
		write(output_file, &data, actual_data_len); // Write to disk

		// Update buffer related variables
		*highest = *expected;
		(*expected)++;

		// Increment sequence number
		(*clientSeqNum)++;

	}

	// Out of Order Data
	else if (seq_num > *expected) {
		// printf("\nOUT OF ORDER DATA\n");
		// printf("     Expected: %d\n", *expected);
		// printf("     Received: %d\n", seq_num);
		// printf("     Highest: %d\n\n", *highest);

		// SREJ expected sequence number #
		uint8_t srej_packet[MAXPDUBUF];
		uint32_t net_expected = htonl(*expected);
		send_buf((uint8_t*)&net_expected, sizeof(net_expected), server, SREJ, clientSeqNum, srej_packet);
		
		// Store into buffer
		window_add(clientWindow, seq_num, data_buf, data_len);

		// printf("Buffered Seq #%d\n", seq_num);

		*highest = seq_num; // Indicates new highest packet in buffer
		(*clientSeqNum)++;
		
		return BUFFER;

	}

	else {
		ackSeqNum = htonl(seq_num + 1); // RR value will be +1 the sequence number
		send_buf((uint8_t *)&ackSeqNum, sizeof(ackSeqNum), server, RR, clientSeqNum, packet);
		(*clientSeqNum)++;
	}

	return RECV_DATA;

}

STATE buffer(int32_t output_file, struct Connection * server, uint32_t * clientSeqNum, struct window *clientWindow, uint32_t *expected, uint32_t *highest)
{
	// printf("\nIn Buffering State\n\n");

	// printf("\nOUT OF ORDER DATA\n");
	// printf("     Expected: %d\n", *expected);
	// printf("     Highest: %d\n\n", *highest);
	uint32_t seq_num = 0;
	uint8_t flag = 0 ;
	int32_t data_len = 0;
	uint8_t data_buf[MAXPDUBUF];


	// Poll for 10 seconds
	if (pollCall(10000) == -1) {
		printf("Timed out waiting for data\n");
		return DONE;
	}

	// Receive data from server
	data_len = recv_buf(data_buf, MAXPDUBUF, server->sk_num, server, &flag, &seq_num);
	// printf("\n\nRecived : %d\n", data_len);
	// printf("Seq: %d\n", seq_num);
	// Check for Flipped bits
	if ((in_cksum((unsigned short *)data_buf, data_len) != 0) || (data_len == CRC_ERROR)) 
	{
		return BUFFER; // Ignore incorrect packet and continue waiting for initial packet.
	}


	if (seq_num < *expected) {
		uint8_t packet[MAXPDUBUF];
		uint32_t net_expected = htonl(*expected);
		uint32_t net_seq = htonl(seq_num+1);
		
		// Send SREJ
		send_buf((uint8_t*)&net_expected, sizeof(net_expected), server, SREJ, clientSeqNum, packet);

		// Send RR
		send_buf((uint8_t*)&net_seq, sizeof(net_seq), server, RR, clientSeqNum, packet);

		return RECV_DATA;

	}



	if (flag == DATA)
	{
		if (data_len != data_packet_len)
		{
			final_packet_len = data_len;
			final_packet_seq = seq_num;
			// printf("Final Packet Length: %d (Seq: %d)\n", final_packet_len, seq_num);
			// printf("Buffer: %s\n", data_buf);
		}
	}
	else if (flag == END_OF_FILE) {
		eof_seq = seq_num;
	}

		
	// In Order Data
	if (seq_num == *expected)	
	{

		


		// Write to Disk
		int actual_data_len = data_len - 7;
		uint8_t data[actual_data_len]; 
		memcpy(data, data_buf + 7, actual_data_len);
		// printf("Printing: %s\n", data);
		// printPDU(data_buf, data_len);

		// printf("ACtual: %d\n", actual_data_len);
		write(output_file, &data, actual_data_len);

		// printf("Writing this much %d\n", actual_data_len);

		window_remove(clientWindow, seq_num); // Invalidate packet in window
		
		// Increment expected
		(*expected)++;

		data[actual_data_len] = '\0';
		

		return FLUSH;
	}

	// Out of Order Data
	else {
		
		// Store into buffer
		window_add(clientWindow, seq_num, data_buf, data_len);
		
		// printf("Buffered Seq #%d\n", seq_num);

		*highest = seq_num;

		// printf("\nOUT OF ORDER DATA\n");
		// printf("     Expected: %d\n", *expected);
		// printf("     Received: %d\n", seq_num);
		// printf("     Highest: %d\n\n", *highest);

		return BUFFER;

	}

	return BUFFER;
}

STATE flush(int32_t output_file, struct Connection * server, uint32_t * clientSeqNum, struct window *clientWindow, uint32_t *expected, uint32_t *highest)
{
	// printf("Flushing\n");

	// Initiate current sequence pointer within buffer
	uint32_t cur_seq = *expected;
	
	printf("\nOUT OF ORDER DATA\n");
	printf("     Expected: %d\n", *expected);
	printf("     Highest: %d\n\n", *highest);

	// Flush data out of buffer (checks for holes in buffer)
	while ((cur_seq == *expected) && (window_isvalid(clientWindow, cur_seq) == 1))
	{
		if ((*expected >= *highest) || ((*expected  < *highest) && (window_isvalid(clientWindow, cur_seq) == 0)))
			break;

		printf("While loop\n");
		// printf("\nOUT OF ORDER DATA\n");
		// printf("     Expected: %d\n", *expected);
		// printf("     Highest: %d\n\n", *highest);
		// printf("     Current Seq: %d\n", cur_seq);

		// Retrieve flushed packet from buffer
		uint8_t *packet = window_get_packet(clientWindow, cur_seq);
		// Write to disk
		uint32_t packet_len = 0;

		if (cur_seq == eof_seq)
		{
			printf("\nFinished Transmission\n");

			uint8_t rr_packet[MAXPDUBUF];
			uint32_t net_expected = htonl(*expected);
			send_buf((uint8_t*)&net_expected, sizeof(net_expected), server, EOF_ACK, clientSeqNum, rr_packet);
			
			exit(0);
		}
		else if (cur_seq == final_packet_seq)
		{
			packet_len = final_packet_len;
		}
		else
			packet_len = data_packet_len;

		int actual_data_len = packet_len - 7;//
		uint8_t data[actual_data_len];
		memcpy(data, packet + 7, actual_data_len);
		write(output_file, &data, actual_data_len);
	

		// Invalidate packet in window
		window_remove(clientWindow, cur_seq); 

		// printf("5\n");


		// Increment expected and current sequence in buffer
		data[actual_data_len] = '\0';
		// printf("Writing Seq #%d: %s\n", cur_seq, data);
		// printf("EOF LEN: %d\n", final_packet_len);
		// printf("EOF SEQ: %d\n", eof_seq);

		// printf("\nOUT OF ORDER DATA\n");
		// printf("     Expected: %d\n", *expected);
		// printf("     Highest: %d\n\n", *highest);
		// printf("     Current Seq: %d\n", cur_seq);
		
		(*expected)++;
		cur_seq++;
		
		
		

	}


	// Go back to RECV_DATA
	if (*expected >= *highest)
	{
		// printf("expected >= highest\n");
		// printf("Data_Packet Len: %d\n", data_packet_len);

		// Send RR for expected
		uint8_t rr_packet[MAXPDUBUF];
		uint32_t net_expected = htonl(*expected);

		if ((*expected) == eof_seq)
		{
			send_buf((uint8_t*)&net_expected, sizeof(net_expected), server, RR, clientSeqNum, rr_packet);
			send_buf((uint8_t*)&net_expected, sizeof(net_expected), server, EOF_ACK, clientSeqNum, rr_packet);
			printf("\nFinished Transmission\n");
			exit(0);
		}
		else
			send_buf((uint8_t*)&net_expected, sizeof(net_expected), server, RR, clientSeqNum, rr_packet);


		uint32_t packet_len = 0;
		if (*expected == final_packet_seq)
		{
			// printf("ASdnsdgbsdiug");
			packet_len = final_packet_len;
		}
		else
			packet_len = data_packet_len;

		// printf("PENIS\n");
		// printf("Expected: %d\nCurrent Seq: %d\n", *expected, cur_seq);
		// printf("Final Packet Len:  %d\n", final_packet_len);
		// printf("Final Packet Seq: %d\n", final_packet_seq);

		// Write to Disk
		uint8_t *packet = window_get_packet(clientWindow, *expected);
		int actual_data_len = packet_len - 7;
		uint8_t data[actual_data_len]; 
		memcpy(data, packet + 7, actual_data_len);

		// if ((*expected - 1) != eof_seq) 
		write(output_file, &data, actual_data_len);

		window_remove(clientWindow, *expected); // Invalidate packet in window
		

		// printf("Writing Seq #%d: %s (%d)\n", *expected, data, actual_data_len);

	
		(*expected)++;

		return RECV_DATA;
		
	}
	

	// Go back to Buffering
	else if ((*expected < *highest) && (window_isvalid(clientWindow, cur_seq) == 0)) {
		// printf("Go back to buffering\n");
		// printf("\nOUT OF ORDER DATA\n");
		// printf("     Expected: %d\n", *expected);
		// printf("     Highest: %d\n\n", *highest);
		// printf("This Conditions\n");
		uint8_t packet[MAXPDUBUF];
		uint32_t net_expected = htonl(*expected);
		
		// Send SREJ
		send_buf((uint8_t*)&net_expected, sizeof(net_expected), server, SREJ, clientSeqNum, packet);

		// Send RR
		send_buf((uint8_t*)&net_expected, sizeof(net_expected), server, RR, clientSeqNum, packet);

	}
	
	return BUFFER;

}


STATE file_ok(int * outputFileFd, char *outputFileName, struct window *clientWindow, int32_t window_size) 
{
	STATE returnValue = DONE;

	if ((*outputFileFd = open(outputFileName, O_CREAT | O_TRUNC | O_WRONLY, 0600)) < 0)
	{
		perror("File open error: ");
		returnValue = DONE;
	}
	else
	{
		// File Exists
		window_create(clientWindow, window_size); // Initialize window
		returnValue = RECV_DATA;
	}
	return returnValue;
}



STATE filename (char * fname, int32_t buf_size, struct Connection * server) {
	int returnValue = START_STATE;
	uint8_t packet[MAXPDUBUF];
	uint8_t flag = 0;
	uint32_t seq_num = 0;
	int32_t recv_check = 0;
	static int retryCount = 0;
	// printf("\nRetry Count: %d\n", retryCount);
	
	if ((returnValue = processSelect(server, &retryCount, START_STATE, FILE_OK, DONE)) == FILE_OK)
	{

		if (pollCall(10000) == -1) {
			printf("Timed out waiting for data\n");
			return DONE;
		}
		
		// Receive establishment Data Packet or Filename Establishment ACK
		recv_check = recv_buf(packet, MAXPDUBUF, server->sk_num, server, &flag, &seq_num);

		// Check for Flipped bits
		if (in_cksum((unsigned short *)packet, recv_check) != 0) 
		{
			retryCount++;
			returnValue = FILENAME; // Ignore incorrect packet and continue waiting for initial packet.
		}
		
		if (recv_check == CRC_ERROR)
		{
			returnValue = START_STATE;
		}
		else if (flag == FNAME_BAD)
		{
			printf("File %s not found\nn", fname);
			exit(1);
		}
		else if (flag == DATA)
		{
			// file yes/no packet lost - instead its a data packet
			returnValue = FILE_OK;
		}

	}

	return returnValue;


}


// Function handles timeouts and retransmissions
STATE processSelect(struct Connection *connection, int *retryCount, STATE TimeoutState, STATE DataState, STATE DoneState) {
    int returnValue = DataState;
    (*retryCount)++;
	
    if (*retryCount > MAX_RETRANS) {
        printf("Sent data %d times, no ACK, client is probably gone\n", MAX_RETRANS);
        returnValue = DoneState;
    } 
	else {
		if (pollCall(1000) != -1) 
		{
            *retryCount = 0;
            returnValue = DataState;
        } 
		else
		{
            printf("We timed out\n");
            returnValue = TimeoutState;
        } 
    }

    return returnValue;
}


int readFromStdin(char * buffer)
{
	char aChar = 0;
	int inputLen = 0;        
	
	// Important you don't input more characters than you have space 
	buffer[0] = '\0';
	printf("Enter data: ");
	while (inputLen < (MAXBUF - 1) && aChar != '\n')
	{
		aChar = getchar();
		if (aChar != '\n')
		{
			buffer[inputLen] = aChar;
			inputLen++;
		}
	}
	
	// Null terminate the string
	buffer[inputLen] = '\0';
	inputLen++;
	
	return inputLen;
}

void checkArgs(int argc, char * argv[])
{

        /* check command line arguments  */
	if (argc != 8)
	{
		printf("usage: rcopy from-filename to-filename window-size buffer-size error-rate remote-machine remote-port\n");
		exit(1);
	}
	if (strlen(argv[1]) > MAXFILELEN)
	{
	    printf("From File length too large\n");
		exit(1);
	}

	if (strlen(argv[2]) > MAXFILELEN)
	{
		printf("To File length too large\n");
		exit(1);
	}
	if (strlen(argv[3]) > MAXWINDOW)
	{
		printf("Window Size too large\n");
		exit(1);
	}
	if (strlen(argv[4]) > 1400)
	{
		printf("Buffer Size too large\n");
		exit(1);
	}
	
}


void writeDisk(int outputFileFd, uint32_t packet_len, uint8_t *packet, struct window *clientWindow, uint32_t seq_num)
{
	int actual_data_len = packet_len - 7;//
	uint8_t data[actual_data_len];
	memcpy(data, packet + 7, actual_data_len);
	write(outputFileFd, &data, actual_data_len);
}

