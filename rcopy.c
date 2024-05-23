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


#define MAXBUF 1400
#define MAXPDUBUF 1407
#define MAXFILELEN 100
#define MAXWINDOW 1073741824
#define MAX_RETRANS 10
#define START_SEQ_NUM 0 


typedef enum State STATE;

enum State
{
	START_STATE, DONE, FILENAME, WAIT_FILE_ACK, FILE_OK, RECV_DATA, 
};


void talkToServer(int socketNum, struct sockaddr_in6 * server);
int readFromStdin(char * buffer);
void checkArgs(int argc, char * argv[]);
void processFile (char * argv[]);
STATE filename (char * fname, int32_t buf_size, struct Connection * server);
STATE processSelect(struct Connection *connection, int *retryCount, STATE TimeoutState, STATE DataState, STATE DoneState);
STATE file_ok(int * outputFileFd, char *outputFileName);
STATE recv_data(int32_t output_file, struct Connection * server, uint32_t * clientSeqNum);


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

        server->sk_num = socketNum; // Set socket number

		bufferSize = htonl(atoi(argv[4])); // Convert buffer size to network order
		windowSize = htonl(atoi(argv[3])); // Convert window size to network order
		fileNameLen = strlen(argv[1]);

		// Build buffer
		memcpy(buf, &bufferSize, 4);
		memcpy(buf + 4, &windowSize, 4);
		memcpy(buf + 8, argv[1], fileNameLen);
		
		send_buf(buf, fileNameLen, server, flag, clientSeqNum, packet);

        (*clientSeqNum)++; // Increment sequence number

		returnValue = FILENAME; 

	}

	return returnValue;
}



int main (int argc, char *argv[])
 {

	checkArgs(argc, argv);	

	sendErr_init(atof(argv[5]), DROP_ON, FLIP_ON, DEBUG_ON, RSEED_OFF); // Set error rate
		
	processFile(argv);
	
	
	return 0;
}


void processFile (char * argv[]) {
	struct Connection *server = (struct Connection *) calloc(1, sizeof(struct Connection));
	uint32_t clientSeqNum = 0;
	int32_t output_file_fd = 0;
	STATE state = START_STATE; // Start State

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
				break;
			
			case FILE_OK:
				state = file_ok(&output_file_fd, argv[2]);
				break;
			
			case RECV_DATA:
				state = recv_data(output_file_fd, server, &clientSeqNum);
				break;

			case WAIT_FILE_ACK:
				break;
		}	
	}
}

STATE recv_data(int32_t output_file, struct Connection * server, uint32_t * clientSeqNum)
{
	uint32_t seq_num = 0;
	uint32_t ackSeqNum = 0;
	uint8_t flag = 0 ;
	int32_t data_len = 0;
	uint8_t data_buf[MAXPDUBUF];
	uint8_t packet[MAXPDUBUF];
	static int32_t expected_seq_num = START_SEQ_NUM;

	// if (pollCall(10000) == -1) {
	// 	printf("Timeout after 10 seconds, server must be gone.\n");
	// 	return DONE;
	// }

	data_len = recv_buf(data_buf, MAXPDUBUF, server->sk_num, server, &flag, &seq_num);

	/* do state RECV_DATA again if there is a crc error (don't send ack, don't write data) */
	if (data_len == CRC_ERROR)
	{
		return RECV_DATA;
	}
	if (flag == END_OF_FILE) 
	{
		printf("Received EOF\n");
	}
	else 
	{
		printf("Received DATA\n");
		// Send ACK
		// ackSeqNum = htonl(seq_num);
		// send_buf();
		// (*clientSeqNum)++;
	}

	if (seq_num == expected_seq_num)
	{
		expected_seq_num++;
		// write(output_file, &data_buf, data_len);
	}
	return RECV_DATA;

}

STATE file_ok(int * outputFileFd, char *outputFileName) 
{
	STATE returnValue = DONE;

	if ((*outputFileFd = open(outputFileName, O_CREAT | O_TRUNC | O_WRONLY, 0600)) < 0)
	{
		perror("File open error: ");
		returnValue = DONE;
	}
	else
	{
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

	
	if ((returnValue = processSelect(server, &retryCount, START_STATE, FILE_OK, DONE)) == FILE_OK)
	{
		recv_check = recv_buf(packet, MAXPDUBUF, server->sk_num, server, &flag, &seq_num);
		
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

	// printf("Next State: %d\n", returnValue);

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
        // int timer = pollCall(1000); // Wait for 1 second        
		// if (timer == connection->sk_num) 
		// {
        //     *retryCount = 0;
        //     returnValue = DataState;
        // } 
		// else if (timer == -1) 
		// {
        //     printf("We timed out\n");
        //     returnValue = TimeoutState;
        // } 
		// else {
        //     // Handle any other unexpected return values from pollCall
        //     printf("Unexpected return value from pollCall: %d\n", timer);
		// 	exit(1);
        // }
        // printf("Socket Timer: %d\n", timer);
		returnValue = DataState;
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





