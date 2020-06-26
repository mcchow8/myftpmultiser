CC = gcc
LIB = -lisal -lm

ifneq ($(shell uname -s), Linux)
    LIB = -lsocket -lnsl -lisal -lm
endif

all: myftpserver myftpclient

myftpserver: myftpserver.c myftp.c
			 ${CC} -o myftpserver myftpserver.c myftp.c -pthread ${LIB}

myftpclient: myftpclient.c myftp.c
			 ${CC} -o myftpclient myftpclient.c myftp.c ${LIB}