#include "myftp.h"
#include <sys/select.h>

//mode: 0 for read, 1 for write
void selectAvailable(fd_set *set, int *fd, int n, int maxfd, int mode)
{
    FD_ZERO(set);
    for (int i = 0; i < n; i++)
        FD_SET(fd[i], set);

    if (mode)
    {
        if (select(maxfd + 1, NULL, set, NULL, NULL) == -1)
        {
            perror("select()");
            exit(1);
        }
    }
    else if (select(maxfd + 1, set, NULL, NULL, NULL) == -1)
    {
        perror("select()");
        exit(1);
    }
}

int checkProgress(int *progress, int size)
{
    for (int i = 0; i < size; i++)
    {
        if (!progress[i])
            return 0;
    }
    return 1;
}

void main_task(in_addr_t *ip, unsigned short *port, char *command, char *filename, int n, int k, int block_size)
{
    fd_set allfdset;
    int count;
    int fd[n];
    int maxfd = 0;
    int online[n];
    int available = 0;
    int filesize, stripes_number;
    Stripe stripe;
    struct sockaddr_in addr[n];
    struct message_s message;

    //connect to multiple server
    for (int i = 0; i < n; i++)
    {
        unsigned int addrlen = sizeof(struct sockaddr_in);
        fd[i] = socket(AF_INET, SOCK_STREAM, 0);

        if (fd[i] == -1)
        {
            perror("socket() error:\n");
            exit(1);
        }

        memset(&addr[i], 0, sizeof(struct sockaddr_in));
        addr[i].sin_family = AF_INET;
        addr[i].sin_addr.s_addr = ip[i];
        addr[i].sin_port = htons(port[i]);
        if (!connect(fd[i], (struct sockaddr *)&addr[i], addrlen))
        {
            maxfd = fd[i];
            online[available++] = i;
        }
    }

    if (!strcmp(command, "list"))
    {
        //ensure at least 1 server is available to get filelist
        if (available < 1)
        {
            fprintf(stderr, "LIST Error: all server unavailable\n");
            exit(1);
        }

        //send LIST_REQUEST
        for (int i = 0; i < available; i++)
        {
            message = createMessage(0xA1, sizeof(struct message_s));
            if ((count = send(fd[online[i]], &message, sizeof(struct message_s), 0)) < 0)
            {
                printf("LIST_REQUEST Error: %s (Errno:%d)\n", strerror(errno), errno);
                exit(1);
            }
        }

        int done = 0;
        while (!done)
        {
            selectAvailable(&allfdset, fd, n, maxfd, 0);

            //receive filelist from the 1st availalbe server
            for (int i = 0; i < available; i++)
            {
                if (FD_ISSET(fd[online[i]], &allfdset))
                {
                    //receive LIST_REPLY
                    if ((count = recv(fd[online[i]], &message, sizeof(struct message_s), 0)) < 0)
                    {
                        printf("LIST_REPLY Error: %s (Errno:%d)\n", strerror(errno), errno);
                        exit(1);
                    };
                    message.length = ntohl(message.length);

                    //receive filelist
                    int length = message.length - sizeof(struct message_s);
                    char filelist[length];
                    if ((count = recv(fd[online[i]], filelist, length, 0)) < 0)
                    {
                        printf("filelist receive Error: %s (Errno:%d)\n", strerror(errno), errno);
                        exit(1);
                    };

                    printf("%s\n", filelist);
                    done = 1;
                    break;
                }
            }
        }
    }
    else if (!strcmp(command, "put"))
    {
        //ensure all servers are available to put file
        if (available < n)
        {
            fprintf(stderr, "PUT Error: some server unavailable\n");
            exit(1);
        }

        //get filesize
        FILE *fp = fopen(filename, "rb");
        if (!fp)
        {
            printf("File Not Exist\n");
            exit(1);
        }
        fseek(fp, 0, SEEK_END);
        filesize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        for (int i = 0; i < n; i++)
        {
            //send PUT_REQUEST
            message = createMessage(0xC1, sizeof(struct message_s) + strlen(filename) + 1);
            if ((count = send(fd[i], &message, sizeof(struct message_s), 0)) < 0)
            {
                printf("PUT_REQUEST Error: %s (Errno:%d)\n", strerror(errno), errno);
                exit(1);
            };

            //send filename
            if ((count = send(fd[i], filename, strlen(filename) + 1, 0)) < 0)
            {
                printf("filename error: %s (Errno:%d)\n", strerror(errno), errno);
                exit(1);
            };

            //receive PUT_REPLY
            if ((count = recv(fd[i], &message, sizeof(struct message_s), 0)) < 0)
            {
                printf("PUT_REPLY Error: %s (Errno:%d)\n", strerror(errno), errno);
                exit(1);
            }
            message.length = ntohl(message.length);

            //send filesize
            message = createMessage(0XFF, sizeof(struct message_s) + filesize);
            if ((count = send(fd[i], &message, sizeof(struct message_s), 0)) < 0)
            {
                printf("FILE_DATA Error: %s (Errno:%d)\n", strerror(errno), errno);
                exit(1);
            };
        }

        int progress[n]; //stores the index of stripes each server is handling
        memset(progress, 0, sizeof(progress));
        stripes_number = calculateStripesNumber(filesize, block_size, k);
        int index = 0; //count the stripes handled

        //initialize stripe
        initStripe(&stripe, n, k, block_size);
        for (int i = 0; i < k; i++)
            fread(stripe.data_block[i], sizeof(unsigned char), block_size, fp);
        encode_data(n, k, &stripe, block_size);

        while (index < stripes_number)
        {
            selectAvailable(&allfdset, fd, n, maxfd, 1);
            for (int i = 0; i < n; i++)
            {
                if (FD_ISSET(fd[i], &allfdset) && !progress[i])
                {
                    //i-th server is handling block[i]
                    unsigned char data[block_size];
                    if (i < k)
                        strcpy(data, stripe.data_block[i]);
                    else
                        strcpy(data, stripe.parity_block[i - k]);
                    if ((count = send(fd[i], data, block_size * sizeof(unsigned char), 0)) < 0)
                    {
                        printf("send file data Error: %s (Errno:%d)\n", strerror(errno), errno);
                        exit(1);
                    }
                    //i-th server send block[i] successfully
                    progress[i] = 1;
                }
            }
            //this stripe is handled
            if (checkProgress(progress, n))
            {
                //initialize stripe
                initStripe(&stripe, n, k, block_size);
                for (int i = 0; i < k; i++)
                    fread(stripe.data_block[i], sizeof(unsigned char), block_size, fp);
                encode_data(n, k, &stripe, block_size);

                index++;
                memset(progress, 0, sizeof(progress));

                printf("Progress: %d / %d\n", index, stripes_number);
            }
        }
        printf("done\n");
    }
    else if (!strcmp(command, "get"))
    {
        //ensure at least k servers are available to get the file
        if (available < k)
        {
            fprintf(stderr, "GET Error: some server unavailable\n");
            exit(1);
        }

        for (int i = 0; i < available; i++)
        {
            //send GET_REQUEST
            message = createMessage(0xB1, sizeof(struct message_s) + strlen(filename) + 1);
            if ((count = send(fd[online[i]], &message, sizeof(struct message_s), 0)) < 0)
            {
                printf("GET_REQUEST Error: %s (Errno:%d)\n", strerror(errno), errno);
                exit(1);
            }

            //send filename
            if ((count = send(fd[online[i]], filename, strlen(filename) + 1, 0)) < 0)
            {
                printf("filename send error: %s (Errno:%d)\n", strerror(errno), errno);
                exit(1);
            }

            //receive GET_REPLY
            if ((count = recv(fd[online[i]], &message, sizeof(struct message_s), 0)) < 0)
            {
                printf("GET_REPLY error: %s (Errno:%d)\n", strerror(errno), errno);
                exit(1);
            }
            message.length = ntohl(message.length);

            //file doesn't exist
            if (message.type == 0xB3)
            {
                printf("File Not Exist\n");
                exit(1);
            }

            //receive filesize
            if ((count = recv(fd[online[i]], &message, sizeof(struct message_s), 0)) < 0)
            {
                printf("filesize error: %s (Errno:%d)\n", strerror(errno), errno);
                exit(1);
            }
            message.length = ntohl(message.length);
            filesize = message.length - sizeof(struct message_s);
        }

        FILE *fp = fopen(filename, "wb");
        int progress[k]; //stores the index of stripes each server is handling
        memset(progress, 0, sizeof(progress));
        stripes_number = calculateStripesNumber(filesize, block_size, k);
        int index = 0; //count the stripes handled

        //initialize stripe
        initStripe(&stripe, n, k, block_size);

        while (index < stripes_number)
        {
            selectAvailable(&allfdset, fd, n, maxfd, 1);
            for (int i = 0; i < k; i++)
            {
                if (FD_ISSET(fd[online[i]], &allfdset) && !progress[i])
                {
                    //receive block
                    int length = block_size;
                    unsigned char payload[RECV_BUFFER_SIZE];
                    int size;
                    while (length > 0)
                    {
                        memset(payload, 0, RECV_BUFFER_SIZE);
                        size = (length > RECV_BUFFER_SIZE) ? RECV_BUFFER_SIZE : length;
                        if ((count = recv(fd[online[i]], payload, size, 0)) < 0)
                        {
                            printf("receive file data Error: %s (Errno:%d)\n", strerror(errno), errno);
                            exit(1);
                        }
                        if (online[i] < k)
                            strcat(stripe.data_block[online[i]], payload);
                        else
                            strcat(stripe.parity_block[online[i] - k], payload);
                        length -= count;
                    }
                    printf("stripe %d: block %d received\n", index, i);
                    progress[i] = 1;
                }
            }
            //this stripe is received
            if (checkProgress(progress, k))
            {
                printf("Decoding stripe %d ...\n", index);
                //recover data
                decode_data(n, k, &stripe, block_size, online);
                for (int i = 0; i < k; i++)
                    fwrite(stripe.data_block[i], sizeof(unsigned char), strlen(stripe.data_block[i]), fp);

                //initialize stripe
                initStripe(&stripe, n, k, block_size);

                index++;
                memset(progress, 0, sizeof(progress));
                printf("Progress: %d / %d\n", index, stripes_number);
            }
        }
        fclose(fp);
        printf("done\n");
    }
    //close all sockets
    for (int i = 0; i < n; i++)
        close(fd[i]);
}

int main(int argc, char **argv)
{
    int n, k, block_size;

    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s clientconfig.txt [list|get|put] [file]\n", argv[0]);
        exit(1);
    }

    if (strcmp(argv[1], "clientconfig.txt"))
    {
        fprintf(stderr, "Error: missing clientconfig.txt\n");
        exit(1);
    }

    FILE *fileclient = fopen(argv[1], "r");
    if (!fileclient)
    {
        fprintf(stderr, "Error: clientconfig.txt not exist\n");
        exit(1);
    }

    fscanf(fileclient, "%d %d %d ", &n, &k, &block_size);
    in_addr_t server_ip[n];
    unsigned short port[n];

    for (int i = 0; i < n; i++)
    {
        char tmp[25];
        fscanf(fileclient, "%s", tmp);
        char *parse = strtok(tmp, ":");
        server_ip[i] = inet_addr(parse);
        parse = strtok(NULL, ":");
        port[i] = atoi(parse);
    }

    if (strcmp(argv[2], "list") && strcmp(argv[2], "get") && strcmp(argv[2], "put"))
    {
        fprintf(stderr, "Command not exist: %s\n", argv[2]);
        exit(1);
    }

    char *filename = NULL;
    if (!strcmp(argv[2], "get") || !strcmp(argv[2], "put"))
        filename = argv[3];

    main_task(server_ip, port, argv[2], filename, n, k, block_size);
    return 0;
}
