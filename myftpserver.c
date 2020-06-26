#include "myftp.h"
#include <pthread.h>

typedef struct
{
    int fd;
    int n;
    int k;
    int block_size;
} arg;

char *getFilepath(char *filename)
{
    int length = strlen(filename) + 6;
    char *filepath = (char *)malloc(length * sizeof(char));
    memset(filepath, 0, length);
    strcpy(filepath, "data/");
    strcat(filepath, filename);
    return filepath;
}

char *createList()
{
    int count = 0;

    char filename[1024][256];
    memset(filename, 0, 1024 * 256);

    DIR *dir = opendir("data");
    struct dirent *file;
    while (file = readdir(dir))
    {
        if (strcmp(file->d_name, ".") && strcmp(file->d_name, ".."))
        {
            //get the real filename
            char *name = strtok(file->d_name, "_");
            char exist = 0;
            //check whether filename exists in list
            for (int i = 0; i < count; i++)
            {
                if (!strcmp(filename[i], name))
                {
                    exist = 1;
                    break;
                }
            }
            //if filename not exist in list, add the filename to the list
            if (!exist)
                strcpy(filename[count++], name);
        }
    }
    closedir(dir);

    //create list from filenames
    char *list = (char *)malloc(sizeof(char) * SEND_BUFFER_SIZE);
    for (int i = 0; i < count; i++)
    {
        strcat(list, filename[i]);
        strcat(list, "\n");
    }
    return list;
}

void createMeta(char *filename, int filesize, int stripes_number)
{
    char name[256];
    strcpy(name, filename);
    strcat(name, "_metadata");
    FILE *metadata = fopen(getFilepath(name), "w");
    fprintf(metadata, "%s %d %d", filename, filesize, stripes_number);
    fclose(metadata);
}

char *makeBlockName(char *filename, int i)
{
    char *blockname = getFilepath(filename);
    char index[5];
    sprintf(index, "%d", i);
    strcat(blockname, "_");
    strcat(blockname, index);
    return blockname;
}

void *thr_func(void *arguments)
{
    pthread_detach(pthread_self());

    arg *args = (arg *)arguments;
    int fd = args->fd;
    int n = args->n;
    int k = args->k;
    int block_size = args->block_size;

    int count;
    struct message_s message;

    pthread_mutex_t lock;
    pthread_mutex_init(&lock, NULL);

    //receive message
    if ((count = recv(fd, &message, sizeof(struct message_s), 0)) < 0)
    {
        printf("Receive Error: %s (Errno:%d)\n", strerror(errno), errno);
        exit(1);
    }
    message.length = ntohl(message.length);

    //receive LIST_REQUEST
    if (message.type == 0xA1)
    {
        //lock this thread before getting filelist
        pthread_mutex_lock(&lock);

        //create filelist
        char payload[SEND_BUFFER_SIZE];
        strcpy(payload, createList());

        //release lock as filelist is created
        pthread_mutex_unlock(&lock);

        //send LIST_REPLY
        message = createMessage(0xA2, sizeof(struct message_s) + strlen(payload) + 1);
        if ((count = send(fd, &message, sizeof(struct message_s), 0)) < 0)
        {
            printf("LIST_REPLY Error: %s (Errno:%d)\n", strerror(errno), errno);
            exit(1);
        }

        //send filelist
        if ((count = send(fd, payload, strlen(payload) + 1, 0)) < 0)
        {
            printf("filelist Error: %s (Errno:%d)\n", strerror(errno), errno);
            exit(1);
        }
    }
    //receive PUT_REQUEST
    else if (message.type == 0xC1)
    {
        //lock this thread before getting filedata
        pthread_mutex_lock(&lock);

        //receive filename
        char filename[message.length - sizeof(struct message_s)];
        if ((count = recv(fd, filename, sizeof(filename), 0)) < 0)
        {
            printf("filename receive error: %s (Errno:%d)\n", strerror(errno), errno);
            exit(0);
        }

        //send PUT_REPLY
        message = createMessage(0xC2, sizeof(struct message_s));
        if ((count = send(fd, &message, sizeof(struct message_s), 0)) < 0)
        {
            printf("PUT_REPLY Error: %s (Errno:%d)\n", strerror(errno), errno);
            exit(1);
        };

        //receive filesize
        if ((count = recv(fd, &message, sizeof(struct message_s), 0)) < 0)
        {
            printf("Receive Error: %s (Errno:%d)\n", strerror(errno), errno);
            exit(1);
        }
        message.length = ntohl(message.length);
        int filesize = message.length - sizeof(struct message_s);

        //calculate stripe number
        int stripes_number = calculateStripesNumber(filesize, block_size, k);
        for (int i = 0; i < stripes_number; i++)
        {
            //make file block
            char *blockname = makeBlockName(filename, i);
            createFile(blockname, block_size, fd);
        }
        printf("all blocks created\n");

        createMeta(filename, filesize, stripes_number);

        pthread_mutex_unlock(&lock);
    }
    //receive GET_REQUEST
    else if (message.type == 0xB1)
    {
        //lock this thread before getting filedata
        pthread_mutex_lock(&lock);

        //receive filename
        char filename[message.length - sizeof(struct message_s)];
        if ((count = recv(fd, filename, sizeof(filename), 0)) < 0)
        {
            printf("filename receive error: %s (Errno:%d)\n", strerror(errno), errno);
            exit(0);
        }

        //try open file meta
        char *metaname = getFilepath(filename);
        strcat(metaname, "_metadata");
        FILE *meta = fopen(metaname, "r");
        if (!meta)
        {
            //send GET_REPLY 0xB3
            message = createMessage(0xB3, sizeof(struct message_s));
            if ((count = send(fd, &message, sizeof(struct message_s), 0)) < 0)
            {
                printf("GET_REPLY 0xB3 Error: %s (Errno:%d)\n", strerror(errno), errno);
                exit(1);
            }
        }
        else
        {
            //read meta and close meta file
            int filesize, stripes_number;
            fscanf(meta, "%s %d %d", filename, &filesize, &stripes_number);
            fclose(meta);

            //send GET_REPLY 0xB2
            message = createMessage(0xB2, sizeof(struct message_s));
            if ((count = send(fd, &message, sizeof(struct message_s), 0)) < 0)
            {
                printf("GET_REPLY 0xB2 Error: %s (Errno:%d)\n", strerror(errno), errno);
                exit(1);
            }

            //send filesize
            message = createMessage(0xFF, sizeof(struct message_s) + filesize);
            if ((count = send(fd, &message, sizeof(struct message_s), 0)) < 0)
            {
                printf("filesize Error: %s (Errno:%d)\n", strerror(errno), errno);
                exit(1);
            }

            //send all blocks
            for (int i = 0; i < stripes_number; i++)
                sendFile(makeBlockName(filename, i), fd);
        }
        pthread_mutex_unlock(&lock);
    }
    close(fd);
}

void main_loop(unsigned short port, int n, int k, int id, int block_size)
{
    int fd, rc;
    struct sockaddr_in addr, tmp_addr;
    unsigned int addrlen = sizeof(struct sockaddr_in);
    pthread_t thread;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
    {
        perror("socket()");
        exit(1);
    }

    long val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(long)) == -1)
    {
        perror("setsockopt()");
        exit(1);
    }

    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        perror("bind()");
        exit(1);
    }
    if (listen(fd, 10) == -1)
    {
        perror("listen()");
        exit(1);
    }

    printf("[To stop the server: press Ctrl + C]\n");

    while (1)
    {
        arg *args = (arg *)malloc(sizeof(arg));
        if ((args->fd = accept(fd, (struct sockaddr *)&tmp_addr, &addrlen)) == -1)
        {
            perror("accept()");
            exit(1);
        }
        args->n = n;
        args->k = k;
        args->block_size = block_size;
        if (rc = pthread_create(&thread, NULL, thr_func, args))
        {
            fprintf(stderr, "error: pthread_create, rc: %d\n", rc);
            exit(1);
        }
    }
}

int main(int argc, char **argv)
{
    int n, k, id, block_size;
    unsigned short port;

    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s serverconfig.txt\n", argv[0]);
        exit(1);
    }

    if (strcmp(argv[1], "serverconfig.txt"))
    {
        fprintf(stderr, "Error: missing serverconfig.txt\n");
        exit(1);
    }

    FILE *fileserver = fopen(argv[1], "r");
    if (!fileserver)
    {
        fprintf(stderr, "Error: serverconfig.txt not exist\n");
        exit(1);
    }

    fscanf(fileserver, "%d %d %d %d %hu", &n, &k, &id, &block_size, &port);

    main_loop(port, n, k, id, block_size);
    return 0;
}
