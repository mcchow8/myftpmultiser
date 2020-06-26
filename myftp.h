#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h> // "struct sockaddr_in"
#include <arpa/inet.h>  // "in_addr_t"
#include <isa-l.h>

#define SEND_BUFFER_SIZE 1048576 //use this when create a buffer for send()
#define RECV_BUFFER_SIZE 65536   //use this when create a buffer for recv()

struct message_s
{
    unsigned char protocol[5]; /* protocol string (5 bytes) */
    unsigned char type;        /* type (1 byte) */
    unsigned int length;       /* length (header + payload) (4 bytes) */
} __attribute__((packed));

typedef struct
{
    int sid;
    unsigned char **data_block;   //pointer to the first data block
    unsigned char **parity_block; //pointer to the first parity block
    uint8_t *encode_matrix;
    uint8_t *errors_matrix;
    uint8_t *invert_matrix;
    uint8_t *table;
} Stripe;

struct message_s createMessage(unsigned char type, unsigned int length);
void createFile(char *filepath, int length, int fd);
void sendFile(char *filepath, int fd);
int calculateStripesNumber(double filesize, double block_size, double k);
void initStripe(Stripe *stripe, int n, int k, int block_size);
void encode_data(int n, int k, Stripe *stripe, size_t block_size);
void decode_data(int n, int k, Stripe *stripe, size_t block_size, int *working_nodes);
