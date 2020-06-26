#include "myftp.h"

struct message_s createMessage(unsigned char type, unsigned int length)
{
    struct message_s message;
    memset(&message, 0, sizeof(struct message_s));
    strcpy(message.protocol, "myftp");
    message.type = type;
    message.length = htonl(length);
    return message;
}

void createFile(char *filepath, int length, int fd)
{
    int count, size;
    FILE *fp = fopen(filepath, "wb");
    unsigned char payload[RECV_BUFFER_SIZE];
    while (length > 0)
    {
        memset(payload, 0, RECV_BUFFER_SIZE);
        size = (length > RECV_BUFFER_SIZE) ? RECV_BUFFER_SIZE : length;
        if ((count = recv(fd, payload, size, 0)) < 0)
        {
            printf("receive file data Error: %s (Errno:%d)\n", strerror(errno), errno);
            exit(1);
        }
        fwrite(payload, sizeof(unsigned char), count, fp);
        length -= count;
    }
    printf("%s created\n", filepath);
    fclose(fp);
}

void sendFile(char *filepath, int fd)
{
    FILE *fp = fopen(filepath, "rb");
    unsigned char payload[SEND_BUFFER_SIZE];
    while (!feof(fp))
    {
        memset(payload, 0, SEND_BUFFER_SIZE);
        size_t bytes_read = fread(payload, sizeof(unsigned char), SEND_BUFFER_SIZE, fp);
        if ((send(fd, payload, bytes_read, 0)) < 0)
        {
            printf("send file data Error: %s (Errno:%d)\n", strerror(errno), errno);
            exit(1);
        }
    }
    printf("%s sent\n", filepath);
    fclose(fp);
}

int calculateStripesNumber(double filesize, double block_size, double k)
{
    return ceil(filesize / (block_size * k));
}

void initStripe(Stripe *stripe, int n, int k, int block_size)
{
    stripe->data_block = (unsigned char **)malloc(sizeof(unsigned char *) * k);
    for (int i = 0; i < k; i++)
    {
        stripe->data_block[i] = (unsigned char *)malloc(sizeof(unsigned char) * block_size);
        memset(stripe->data_block[i], 0, sizeof(unsigned char) * block_size);
    }
    stripe->parity_block = (unsigned char **)malloc(sizeof(unsigned char *) * (n - k));
    for (int i = 0; i < n - k; i++)
    {
        stripe->parity_block[i] = (unsigned char *)malloc(sizeof(unsigned char) * block_size);
        memset(stripe->parity_block[i], 0, sizeof(unsigned char) * block_size);
    }
    // allocate space for matrix
    stripe->encode_matrix = malloc(sizeof(uint8_t) * (n * k));
    stripe->errors_matrix = malloc(sizeof(uint8_t) * (k * k));
    stripe->invert_matrix = malloc(sizeof(uint8_t) * (k * k));
    stripe->table = malloc(sizeof(uint8_t) * (32 * k * (n - k)));
}

void encode_data(int n, int k, Stripe *stripe, size_t block_size)
{
    // Generate encode matrix
    gf_gen_rs_matrix(stripe->encode_matrix, n, k);

    // Generates the expanded tables needed for fast encoding
    ec_init_tables(k, n - k, &stripe->encode_matrix[k * k], stripe->table);

    // Actually generated the error correction codes
    ec_encode_data(block_size, k, n - k, stripe->table, stripe->data_block, stripe->parity_block);
}

void decode_data(int n, int k, Stripe *stripe, size_t block_size, int *working_nodes)
{
    // Generate encode matrix
    gf_gen_rs_matrix(stripe->encode_matrix, n, k);

    // Generate error matrix
    for (int i = 0; i < k; i++)
        for (int j = 0; j < k; j++)
            stripe->errors_matrix[i * k + j] = stripe->encode_matrix[working_nodes[i] * k + j];

    // Generate invert matrix
    gf_invert_matrix(stripe->errors_matrix, stripe->invert_matrix, k);

    //find the online servers
    int online[n];
    memset(online, 0, sizeof(online));
    for (int i = 0; i < k; i++)
        online[working_nodes[i]] = 1;

    // Generate decode matrix
    uint8_t *decode_matrix = (uint8_t *)malloc(sizeof(uint8_t) * (k * k));
    memset(decode_matrix, 0, sizeof(decode_matrix));
    for (int i = 0, index = 0; index < n - k; i++)
        if (!online[i])
        {
            for (int j = 0; j < k; j++)
                decode_matrix[index * k + j] = stripe->invert_matrix[i * k + j];
            index++;
        }

    // Generates the expanded tables needed for fast encoding
    ec_init_tables(k, n - k, decode_matrix, stripe->table);

    // find existing & missing blocks
    unsigned char *exist[k], *miss[n - k];
    for (int i = 0, index1 = 0, index2 = 0; i < n; i++)
    {
        unsigned char *block = (i < k) ? stripe->data_block[i] : stripe->parity_block[i - k];
        if (online[i] && index1 < k)
            exist[index1++] = block;
        else
            miss[index2++] = block;
    }

    ec_encode_data(block_size, k, n - k, stripe->table, exist, miss);
}
