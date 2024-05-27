	/* Server side - UDP Code				    */
	/* By Hugh Smith	4/1/2017	*/

	#include <stdio.h>
	#include <stdlib.h>
	#include <unistd.h>
	#include <string.h>
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <sys/uio.h>
	#include <sys/time.h>
	#include <sys/wait.h>
	#include <unistd.h>
	#include <fcntl.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <sys/resource.h>
	#include <signal.h>

	#include "gethostbyname.h"
	#include "networks.h"
	#include "safeUtil.h"
	#include "pdu.h"
	#include "pollLib.h"
	#include "pdu.h"
	#include "window.h"

	#define MAXBUF 1400
	#define MAXPDUBUF 1407
	#define MAX_FILE 100
	#define START_SEQ_NUM 1
	#define NOTFILENAME 15
	#define MAX_RETRANS 10

	int32_t final_packet_len = 0;
	int32_t final_packet_seq = 0;
	

	typedef enum State STATE;

	enum State
	{
		START, DONE, FILENAME, SEND_DATA, WAIT_ON_EOF_ACK, WAIT_ON_ACK, TIMEOUT_ON_ACK, TIMEOUT_ON_EOF_ACK
	};

	void process_client(int32_t serverSocketNumber, uint8_t *buf, int32_t recv_len, struct Connection * server);
	void process_server(int serverSocketNumber, float error_rate);
	int checkArgs(int argc, char *argv[]);
	void handleZombies(int sig);
	STATE wait_on_ack(struct Connection * client, struct window* input_window, uint32_t *last_seq_num, int32_t packet_len, uint32_t * seq_num, int * finished, int32_t *data_packet_len);
	STATE processSelect(struct Connection *connection, int *retryCount, STATE TimeoutState, STATE DataState, STATE DoneState, struct window* input_window, int * finished);
	STATE filename(struct Connection * client, uint8_t * buf, int32_t recv_len, int32_t * data_file, int32_t * buf_size, int32_t * window_size, struct window *serverWindow, int32_t *data_packet_len);
	STATE wait_on_eof_ack(struct Connection * client, struct window* input_window, uint32_t last_seq_num, int32_t *eof_len);
	STATE timeout_on_ack(struct Connection * client, uint8_t * packet, struct window *serverWindow, int32_t *data_packet_len);
	STATE timeout_on_eof_ack (struct Connection * client, uint8_t * packet, int32_t packet_len);
	STATE send_srej(struct Connection * client, struct window* input_window, uint8_t *srej_packet, uint32_t data_packet_len, uint32_t * seq_num);
	STATE send_data (struct Connection *client, uint8_t * packet, int32_t * packet_len, int32_t 
	data_file, int buf_size, uint32_t * seq_num, uint32_t *last_seq_num, struct window *serverWindow, int32_t *eof_len, int * finished, int32_t *data_packet_len);


	// // Main control for server processes
	void process_server(int serverSocketNumber, float error_rate) {
		pid_t pid = 0;
		uint8_t buf[MAXPDUBUF]; // 1407
		struct Connection *client = (struct Connection *) calloc(1, sizeof(struct Connection));

		uint8_t flag = 0;
		uint32_t seq_num = 0;
		int recv_len = 0;
		
		signal(SIGCHLD, handleZombies); // Clean up before fork()

		while (1)
		{
			// Wait for establishment packet from incoming clients (window size, filename, & buffer size)
			recv_len = recv_buf(buf, MAXPDUBUF, serverSocketNumber, client, &flag, &seq_num);
			
			// Check for Flipped bits
			if (in_cksum((unsigned short *)buf, recv_len) != 0) 
			{
				continue; // Ignore incorrect packet and continue waiting for initial packet.
			}

			if (recv_len != CRC_ERROR) 
			{
				// Error
				if ((pid = fork()) < 0)
				{
					perror("fork");
					exit(-1);
				}	

				// Child process 
				if (pid == 0)
				{
					printf("Child fork() - child pid: %d\n", getpid());
					printf("Error: %f\n", error_rate);
					sendErr_init(error_rate, DROP_ON, FLIP_ON, DEBUG_ON, RSEED_ON);
					process_client(serverSocketNumber, buf, recv_len, client);
					exit(0);
				}
			}

		}
	}


	void process_client(int32_t serverSocketNumber, uint8_t *buf, int32_t recv_len, struct Connection * client) 
	{
		STATE state = START;
		int32_t data_file = 0;
		int32_t packet_len = 0;
		int32_t eof_len = 0;
		uint8_t packet[MAXPDUBUF];
		int32_t buf_size = 0;
		int32_t window_size = 0;
		uint32_t seq_num = START_SEQ_NUM;
		uint32_t last_seq_num = 0;
		struct window *serverWindow = (struct window *) calloc(1, sizeof(struct window));

		int finished = 0; // Indiates EOF has been transmitted (Window is Closed)
		int32_t data_packet_len = 0;
		// int32_t final_packet_len = 0;
		// int32_t final_packet_seq = 0;

		while (state != DONE)
		{
			switch (state)
			{
				case START:
					state = FILENAME;
					break;
				
				case FILENAME:
					state = filename(client, buf, recv_len, &data_file, &buf_size, &window_size, serverWindow, &data_packet_len);
					break;
				
				case SEND_DATA:
					state = send_data(client, packet, &packet_len, data_file, buf_size, &seq_num, &last_seq_num, serverWindow, &eof_len, &finished, &data_packet_len);
					break;

				case WAIT_ON_ACK:
					state = wait_on_ack(client, serverWindow, &last_seq_num, packet_len, &seq_num, &finished, &data_packet_len);
					break;

				case WAIT_ON_EOF_ACK:
					state = wait_on_eof_ack(client, serverWindow, last_seq_num, &eof_len);
					break;

				case TIMEOUT_ON_ACK:
					state = timeout_on_ack(client, packet, serverWindow, &data_packet_len);
					break;
				
				case TIMEOUT_ON_EOF_ACK:
					state = timeout_on_eof_ack(client, packet, packet_len);
					break;

				case DONE:
					printf("Done\n");
					break;
			}
		}
	}


	// Retransmission of lowest packet in window
	STATE timeout_on_ack(struct Connection * client, uint8_t * packet, struct window *serverWindow, int32_t *data_packet_len) 
	{
		// Resent lowest packet in window buffer
		uint8_t flag = DATA_TIMEOUT;
		uint8_t *retransmission = window_get_lower(serverWindow);

		uint32_t seq_num = 0;
		memcpy(&seq_num, retransmission, 4);
		seq_num = ntohl(seq_num);
		// printf("Retransmitting Seq: %d\n", seq_num);

		uint32_t new_packet_len = 0;
		if (seq_num == final_packet_seq) {
			new_packet_len = final_packet_len;
		}
		else {
			new_packet_len = *data_packet_len;
		}


		memcpy(retransmission + 6, &flag, 1);

		// Recalculate checksum
		uint16_t checksum = 0;
		memcpy(retransmission + 4, &checksum, 2); // Clear old checksum
		checksum = in_cksum((unsigned short *)retransmission, new_packet_len); // Calculate new checksum
		memcpy(retransmission + seqNumLen, &checksum, chkSumLen);

		// printf("%d\n", serverWindow->lower);

		safeSendto(client->sk_num, retransmission, new_packet_len, 0, (struct sockaddr *)&client->address, sizeof(client->address));



		return WAIT_ON_ACK;
	}


	STATE timeout_on_eof_ack (struct Connection * client, uint8_t * packet, int32_t packet_len)
	{
		safeSendto(client->sk_num, packet, packet_len, 0, (struct sockaddr *)&client->address, sizeof(client->address));
		return WAIT_ON_EOF_ACK;
	}


	STATE filename(struct Connection * client, uint8_t * buf, int32_t recv_len, int32_t * data_file, int32_t * buf_size, int32_t * window_size, struct window *serverWindow, int32_t *data_packet_len)
	{
		uint32_t seqNum = 0; 
		int fileNameLen = 0;

		uint8_t response[1];
		char fname[MAX_FILE];
		STATE returnValue = DONE;

		// Extract Buffer Size
		memcpy(buf_size, buf + 7, 4);
		*buf_size = ntohl(*buf_size);
		*data_packet_len = 7 + *buf_size;

		// Extract Window Size
		memcpy(window_size, buf+ 11, 4);
		*window_size = ntohl(*window_size);

		// Extrace File Name
		memcpy(fname, buf + NOTFILENAME, recv_len - NOTFILENAME);
		int fileLen = recv_len - NOTFILENAME;
		fname[fileLen] = '\0';

		// Create socket associated with client
		client->sk_num = safeGetUdpSocket();
		
		// Setup Poll table for poll()
		setupPollSet();
		addToPollSet(client->sk_num);
		
		if (((*data_file) = open(fname, O_RDONLY)) < 0) 
		{
			send_buf(response, fileNameLen, client, FNAME_BAD, &seqNum, buf);
			returnValue = DONE;
		}

		else 
		{
			send_buf(response, fileNameLen, client, FNAME_OK, &seqNum, buf);
			returnValue = SEND_DATA;
		}

		// Initialize Window Buffer
		window_create(serverWindow, *window_size);
		// window_print(serverWindow);
		
		return returnValue;
	}

	STATE send_data (struct Connection *client, uint8_t * packet, int32_t * packet_len, int32_t data_file, int buf_size, uint32_t * seq_num, uint32_t *last_seq_num,  struct window *serverWindow, int32_t *eof_len, int * finished, int32_t *data_packet_len)
	{
		uint8_t buf[MAXPDUBUF];
		int32_t len_read = 0;
		STATE returnValue = DONE;

		// Check if window is full
		if (window_full(serverWindow) == 1) {
			return WAIT_ON_ACK; // Wait for RR
		}

		len_read = read(data_file, buf, buf_size);

		buf[buf_size] = '\0';

		switch (len_read)
		{
			case (-1):
				perror("send_data, read error");
				returnValue = DONE;
				break;
			case (0):
				if (!(*finished)) 
				{
					(*packet_len) = send_buf(buf, 1, client, END_OF_FILE, seq_num, packet);
					window_add(serverWindow, *seq_num, packet, *packet_len);
					window_CURUpdate(serverWindow);
					// window_print(serverWindow);

					*last_seq_num = *seq_num; // Retrieve last sequence number for reference
					*eof_len = *packet_len;
					// printf("Last Seq Num: %d (%d)\n", *last_seq_num, *eof_len);
					*finished = 1;
				}

				returnValue = WAIT_ON_ACK;
				break;
			default:

				(*packet_len) = send_buf(buf, len_read, client, DATA, seq_num, packet);
				// printPDU(packet, *packet_len);
				
				// Store final packet length that may not be size of buffer
				if (*packet_len != *data_packet_len) {
					final_packet_len = *packet_len;
					final_packet_seq = *seq_num;
				}

				// Store sent packet into buffer until receiving RR
				window_add(serverWindow, *seq_num, packet, *packet_len);
				window_CURUpdate(serverWindow);
				// window_print(serverWindow);

				// Increment Sequence Number
				(*seq_num)++;

				returnValue = SEND_DATA;
				break;
				
		}
		return returnValue;
	}

	STATE wait_on_ack(struct Connection * client, struct window* input_window, uint32_t *last_seq_num, int32_t packet_len, uint32_t * cur_seq, int * finished, int32_t *data_packet_len)
	{
		STATE returnValue = DONE;
		uint32_t crc_check = 0;
		uint8_t buf[MAXPDUBUF];
		uint32_t len = MAXPDUBUF;
		uint8_t flag = 0;
		uint32_t seq_num = 0;
		static int retryCount = 0;


		// Check for timeout
		if ((returnValue = processSelect(client, &retryCount, TIMEOUT_ON_ACK, SEND_DATA, DONE, input_window, finished
		)) == SEND_DATA)
		{

			// Receive RR buffer from client
			crc_check = recv_buf(buf, len, client->sk_num, client, &flag, &seq_num);

			// Check for flipped bits/corrupted packets
			if (in_cksum((unsigned short *)buf, crc_check) != 0) 
			{
				return WAIT_ON_ACK; // Ignore incorrect packet and continue waiting for initial packet.
			}


		
			if(crc_check == CRC_ERROR)
			{
				returnValue = WAIT_ON_ACK;
			}
			else if (flag == SREJ)
			{
				if (*finished == 0) {
					send_srej(client, input_window, buf, *data_packet_len, cur_seq);				
					returnValue = SEND_DATA;
				}
				else {
					send_srej(client, input_window, buf, *data_packet_len, cur_seq);				
					returnValue = SEND_DATA;
				}
			}
			else if (flag == EOF_ACK)
			{
				printf("\nFinished Transmission\n");
				returnValue = DONE;
			}
			else if (flag != RR)
			{
				printf("In wait_on_ack but its not an RR flag (this should never happen) is: %d\n", flag);
				returnValue = DONE;
			}
		}


		// Successful Transmission
		if ((returnValue == SEND_DATA) && (flag == RR))
		{
			uint32_t rr_seq = 0;
			memcpy(&rr_seq, buf+7, 4);
			rr_seq = ntohl(rr_seq);	

			if (rr_seq == final_packet_seq + 1) 
			{
				// printf("Penis\n");
				return WAIT_ON_EOF_ACK;
			}		

			window_slide(input_window, rr_seq);
			window_remove(input_window, rr_seq);
			// window_print(input_window);
		}
		

		

		return returnValue;

	}

	STATE send_srej(struct Connection * client, struct window* input_window, uint8_t *srej_packet, uint32_t data_packet_len, uint32_t * seq_num) {
		uint8_t flag = SREJ_RETRAN;
		uint32_t packet_len = 0;

		// Get sequence number SREJ'd
		uint32_t srej_seq = 0;
		memcpy(&srej_seq, srej_packet + 7, 4);
		srej_seq = ntohl(srej_seq);

		// printf("\nSREJ_SEQ: %d\n", srej_seq);

		uint8_t *retransmission = window_get_packet(input_window, srej_seq);
		memcpy(retransmission + 6, &flag, 1);
		
		printPacket(retransmission, 12);


		if (srej_seq == final_packet_seq)
		{
			packet_len = final_packet_len;
		}
		else {
			packet_len = data_packet_len;
		}


		// Recalculate checksum
		uint16_t checksum = 0;
		memcpy(retransmission + 4, &checksum, 2); // Clear old checksum
		checksum = in_cksum((unsigned short *)retransmission, packet_len); // Calculate new checksum
		memcpy(retransmission + seqNumLen, &checksum, chkSumLen);
		
		// printf("Sending SREJ with %d (%d)\n", srej_seq, packet_len);
		// printf("Current: %d\n", *seq_num);
		if (srej_seq == *seq_num)
			return SEND_DATA;

		
		// printf("data_packet: %d\n", packet_len);
		// printf("final packet: %d\n", final_packet_len);
		// printf("final seq: %d\n", final_packet_seq);
		safeSendto(client->sk_num, retransmission, packet_len, 0, (struct sockaddr *)&client->address, sizeof(client->address));

		return WAIT_ON_ACK;


		
	}

	STATE wait_on_eof_ack(struct Connection * client, struct window* input_window, uint32_t last_seq_num, int32_t *eof_len)
	{
		uint32_t crc_check = 0;
		uint8_t buf[MAXPDUBUF];
		int32_t len = MAXPDUBUF;
		uint8_t flag = 0;
		uint32_t seq_num = 0;
		static int retryCount = 0;


		uint8_t *eof_packet = window_get_packet(input_window, last_seq_num);

		while(1) 
		{
			if (retryCount > MAX_RETRANS - 1) 
			{
				printf("Sent data %d times, no ACK, client is probably gone\n", MAX_RETRANS);
				exit(0);
			} 

			safeSendto(client->sk_num, eof_packet, *eof_len, 0, (struct sockaddr *)&client->address, sizeof(client->address));
			
			if (pollCall(1000) == -1)
			{
				retryCount++;
				continue;
			} 
			else 
			{
				crc_check = recv_buf(buf, len, client->sk_num, client, &flag, &seq_num);
				// Check for flipped bits/corrupted packets
				if (in_cksum((unsigned short *)buf, crc_check) != 0) 
				{	
					retryCount++;
					continue; // Ignore incorrect packet and continue waiting for initial packet.
				}
				else if (flag == EOF_ACK) {
					return DONE;
				}
				
			}
				
		}
		
	}



	int main ( int argc, char *argv[]  )
	{ 
		uint32_t serverSocketNumber = 0;			
		int portNumber = 0;

		portNumber = checkArgs(argc, argv);	// Check if command call format is correct
			
		serverSocketNumber = udpServerSetup(portNumber); // Setup UDP server

		process_server(serverSocketNumber, atof(argv[1]));
		
		return 0;
	}



	int checkArgs(int argc, char *argv[])
	{
		// Checks args and returns port number
		int portNumber = 0;

		if (argc > 3 || argc < 2)
		{
			fprintf(stderr, "Usage %s [error rate] [optional port number]\n", argv[0]);
			exit(-1);
		}
		
		if (argc == 3)
		{
			portNumber = atoi(argv[2]);
		}
		
		return portNumber;
	}

	void handleZombies(int sig) 
	{
		int stat = 0;
		while (waitpid(-1, &stat, WNOHANG) > 0);
	}


	// Function handles timeouts and retransmissions
	STATE processSelect(struct Connection *connection, int *retryCount, STATE TimeoutState, STATE DataState, STATE DoneState, struct window* input_window, int * finished) {
		int returnValue = DataState;
		(*retryCount)++;
		
		if (*retryCount > MAX_RETRANS) {
			printf("Sent data %d times, no ACK, client is probably gone\n", MAX_RETRANS);
			returnValue = DoneState;
		} 
		else {

			// Window closed
			if ((window_full(input_window) == 1) || (*finished == 1)) { 
				// printf("Window is full\n");
				int timer = pollCall(1000); // Wait for 1 second        
				
				if (timer != -1) 
				{
					*retryCount = 0;
					returnValue = DataState;
				} 

				else if (timer == -1) 
				{
					// printf("We timed out\n");
					returnValue = TimeoutState;
				} 
				else {
					// Handle any other unexpected return values from pollCall
					printf("Unexpected return value from pollCall: %d\n", timer);
					exit(1);
				}
			}

			// Window not closed
			else {
				int timer = pollCall(0);

				if (timer != -1) 
				{
					*retryCount = 0;
					returnValue = DataState;
				} 

				else if (timer == -1) 
				{
					// printf("We timed out\n");
					returnValue = TimeoutState;
				} 
				else {
					// Handle any other unexpected return values from pollCall
					printf("Unexpected return value from pollCall: %d\n", timer);
					exit(1);
				}
			}
		}

		return returnValue;
	}