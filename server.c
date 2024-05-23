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

#define MAXBUF 1400
#define MAXPDUBUF 1407
#define MAX_FILE 100
#define START_SEQ_NUM 0
#define NOTFILENAME 15

typedef enum State STATE;

enum State
{
	START, DONE, FILENAME, SEND_DATA, WAIT_ON_EOF_ACK, WAIT_ON_ACK
};

void process_client(int32_t serverSocketNumber, uint8_t *buf, int32_t recv_len, struct Connection * server);
void process_server(int serverSocketNumber);
int checkArgs(int argc, char *argv[]);
void handleZombies(int sig);
STATE filename(struct Connection * client, uint8_t * buf, int32_t recv_len, int32_t * data_file, int32_t * buf_size, int32_t * window_size);
STATE send_data (struct Connection *client, uint8_t * packet, int32_t * packet_len, int32_t 
data_file, int buf_size, uint32_t * seq_num);


// // Main control for server processes
void process_server(int serverSocketNumber) {
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
		pollCall(0);
		recv_len = recv_buf(buf, MAXPDUBUF, serverSocketNumber, client, &flag, &seq_num);

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
	uint8_t packet[MAXPDUBUF];
	int32_t buf_size = 0;
	int32_t window_size = 0;
	uint32_t seq_num = START_SEQ_NUM;

	while (state != DONE)
	{
		switch (state)
		{
			case START:
				state = FILENAME;
				break;
			
			case FILENAME:
				state = filename(client, buf, recv_len, &data_file, &buf_size, &window_size);
				break;
			
			case SEND_DATA:
				state = send_data(client, packet, &packet_len, data_file, buf_size, &seq_num);
				break;

			case WAIT_ON_ACK:
				break;

			case WAIT_ON_EOF_ACK:
				break;

			case DONE:
				exit(1);
				break;
		}
	}
}

STATE filename(struct Connection * client, uint8_t * buf, int32_t recv_len, int32_t * data_file, int32_t * buf_size, int32_t * window_size)
{
	int send_check = 0;
	uint32_t seqNum = 0; 
	int fileNameLen = 0;

	uint8_t response[1];
	char fname[MAX_FILE];
	STATE returnValue = DONE;

	// Extract filename from packet
	memcpy(buf_size, buf + 7, 4);
	*buf_size = ntohl(*buf_size);

	memcpy(window_size, buf+ 11, 4);

	memcpy(fname, buf + NOTFILENAME, recv_len - NOTFILENAME);
	int fileLen = recv_len - NOTFILENAME;
	fname[fileLen] = '\0';

	client->sk_num = safeGetUdpSocket();
	
	if (((*data_file) = open(fname, O_RDONLY)) < 0) 
	{
		send_check = send_buf(response, fileNameLen, client, FNAME_BAD, &seqNum, buf);
		returnValue = DONE;
	}

	else 
	{
		send_check = send_buf(response, fileNameLen, client, FNAME_OK, &seqNum, buf);
		returnValue = SEND_DATA;
	}
	
	return returnValue;
}

STATE send_data (struct Connection *client, uint8_t * packet, int32_t * packet_len, int32_t data_file, int buf_size, uint32_t * seq_num)
{
	uint8_t buf[MAXPDUBUF];
	int32_t len_read = 0;
	STATE returnValue = DONE;

	len_read = read(data_file, buf, buf_size);

	switch (len_read)
	{
		case (-1):
			perror("send_data, read error");
			returnValue = DONE;
			break;
		case (0):
			(*packet_len) = send_buf(buf, 1, client, END_OF_FILE, seq_num, packet);
			returnValue = WAIT_ON_EOF_ACK;
			printf("End of File\n");
			exit(0);
			break;
		default:
			(*packet_len) = send_buf(buf, len_read, client, DATA, seq_num, packet);
			(*seq_num++);
			printf("SEQUENCE NUMBER : %d\n", *seq_num);
			returnValue = SEND_DATA;
			// returnValue = WAIT_ON_ACK;
			break;
			
	}
	return returnValue;
}



int main ( int argc, char *argv[]  )
{ 
	
	uint32_t serverSocketNumber = 0;			
	int portNumber = 0;

	portNumber = checkArgs(argc, argv);	// Check if command call format is correct
		
	serverSocketNumber = udpServerSetup(portNumber); // Setup UDP server

	process_server(serverSocketNumber);
	
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
