#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#define errExit(msg)    do { perror(msg); exit(EXIT_FAILURE);} while (0)


#define RATE 2662
#define BLOCK 640
#define DATAPORTION 13312
#define DATABLOCK 3328  // 13312 / 4 == 3328
#define MAXCLIENTS 1000

typedef struct clientParameters
{
    int fd;
    int numOfRequestedBlocks;
    struct sockaddr_in clientAddr;
}clientParameters;

typedef struct dataContainer
{
    float frequency;
    int port;
    char* address;
    int server_fd;
    struct sockaddr_in server;
    int epollfd;
    struct epoll_event ev;
    int timerfd;
    int toRead;
    int numOfBlocks;    //number of blocks of 3328 bytes to send to all clients
    int numOfClients;
    int generatedBytes;
    
    //circle buffer from previous task
    int* ringBuffer;
    int firstUsed;
    int lastUsed;
    int size;

}dataContainer;

struct sigaction  sa;    
struct sigevent   sev;

//main logic of program
void createServer(dataContainer* d);
int createChild(dataContainer* d);
void resourceDistribution(dataContainer* d);    //epoll_wait + for loop on every ready client


//functions inside for loop in resourceDistribution function
void acceptNewClient(dataContainer* d);
void generateReport(dataContainer* d, int NumOfClients);
void checkClient(dataContainer* d, struct epoll_event* events, int iter , clientParameters* cd );
int operateOnClient( struct epoll_event* events, int iter, dataContainer* d ); 

//function just after accept function
void placeClientInRingBuffOrEpoll(dataContainer* d, int client_sock);
void addClientToEpoll(dataContainer* d, int flags, int fdToAdd, int checkAddr, struct sockaddr_in addr);

//addictional functions for preparing structures/removing clients
void createSetEpoll(dataContainer* d);
void armTimer(dataContainer* d);
void disconnectFromServer(clientParameters* cd, dataContainer* d);

// functions in child (magazine/resources creator)
void child(int toWrite, dataContainer* d);
int insertBlock(int fdToWrite, char c);

//parse functions
int parseInt(char* arr );
float parseFloat(char* arr);
void parseArguments(int argc, char** argv, dataContainer* d);
void parseAddress(char* arg, dataContainer* d);

// ring buffer functions
void addElem(dataContainer* b, int elem);
int  removeFirstElem(dataContainer* b);
int draw(dataContainer b, int e);

/*
Dziwne zjawiska pogodowe:
dla ./producent -p 20 5566
i przy 100 klientach o wszystkich parametrach 1, klientów nie udaje się obsłużyć
wynika to najprawdopodobniej z faktu, że czekają w buforze zbyt długo i to powoduje tracenie zasobów z magazynu
szybciej niż uzyskiwanie nowych.
Wystarczy albo zmniejszyć liczbę klientów albo zwiększyć tempo w producencie
*/


int main(int argc, char** argv)
{
    dataContainer d={0};
    parseArguments(argc,argv, &d);
    parseAddress(argv[argc-1], &d);
    createServer(&d);
    signal(SIGCHLD,SIG_IGN);  //I don't want to have zombie
    d.toRead = createChild(&d);
    armTimer(&d);
    createSetEpoll(&d);
    resourceDistribution(&d);
    

    if(d.ringBuffer != NULL)
        free(d.ringBuffer);

    close(d.toRead);
    close(d.server_fd);

    return 0;
}



void createServer(dataContainer* d)
{
    int on = 1;
    
    if ((d->server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) 
        errExit("socket");

    if( setsockopt(d->server_fd, SOL_SOCKET,  SO_REUSEADDR, (char *)&on, sizeof(on)) <0)
        errExit("setsockopt");

    d->server.sin_family = AF_INET; 
    d->server.sin_port = htons(d->port); 
    if(inet_pton(AF_INET, d->address, &d->server.sin_addr)==0)  
        errExit("inet_pton");

    if (bind(d->server_fd, (struct sockaddr *)&d->server, sizeof(d->server))<0)
        errExit("bind");

    if (listen(d->server_fd, MAXCLIENTS) < 0) 
        errExit("listen");

}

int createChild(dataContainer* d)
{
    int fd[2];
    if(pipe2(fd, O_NONBLOCK) == -1)
        errExit("pipe");

    pid_t pid = fork();
    if(pid == -1)
        errExit("fork");
    else if( pid == 0)
    {
        close(fd[0]);   //close read end
        signal(SIGPIPE,SIG_IGN);    
        child(fd[1], d);
        close(fd[1]);   //close write end
        exit(EXIT_SUCCESS);
    }
    close(fd[1]);
    return fd[0];
}

void resourceDistribution(dataContainer* d)
{
    struct epoll_event events[MAXCLIENTS];
    int nfds;
    int str;
	while( 1 )
	{
        if( ioctl(d->toRead, FIONREAD, &str) == -1)
            errExit("ioctl");
        
        while((d->size > 0) && (str >( (d->numOfBlocks * DATABLOCK)  + DATAPORTION)) )
        {
            int sockfd = removeFirstElem(d);
            struct sockaddr_in addr = {0};
            socklen_t addLen = sizeof(addr);
            if(getpeername(sockfd, (struct sockaddr* )&addr, &addLen) == -1)
                errExit("getpeername");
            addClientToEpoll(d, (EPOLLOUT | EPOLLRDHUP) , sockfd, 1, addr );
            d->numOfBlocks +=4;
        }

        if( (nfds = epoll_wait(d->epollfd, events, MAXCLIENTS, 0)) == -1)
            errExit("epoll_wait");

        for(int i=0; i< nfds; i++)
        {
            clientParameters* cd = events[i].data.ptr;
            if(cd->fd == d->server_fd)
                acceptNewClient(d);
            else if(cd->fd == d->timerfd && events[i].events & EPOLLIN)
                generateReport(d, d->numOfClients);  
            else
                checkClient(d,events, i, cd);  
        }
	}
}

void acceptNewClient(dataContainer* d)
{
    struct sockaddr_in client;
    int client_sock;
    int c = sizeof(struct sockaddr_in);
    if((client_sock = accept(d->server_fd, (struct sockaddr *)&client, (socklen_t*)&c)) == -1)
        errExit("accept");
    
    placeClientInRingBuffOrEpoll(d, client_sock);
}

void generateReport(dataContainer* d, int NumOfClients)
{
    struct timespec ts = {0};
    int pipeCapacity = fcntl(d->toRead, F_GETPIPE_SZ);
    int str;
    if( ioctl(d->toRead, FIONREAD, &str) == -1)
        errExit("ioctl");
    uint64_t numExp;
    if ((numExp = read(d->timerfd, &numExp, sizeof(uint64_t)) != sizeof(uint64_t)) )            
        errExit("read");
    if(clock_gettime(CLOCK_REALTIME, &ts)== -1)
        errExit("clock_gettime");
    fprintf(stderr, "TS: %ld.%ld bytes in pipe: %d,  %.2f%%; number of connected clients %d flow %d\n",ts.tv_sec, ts.tv_nsec, 
    str, ( ( (float)str )/( (float)pipeCapacity ) )*100, NumOfClients, str - d->generatedBytes );
    d->generatedBytes = str;
}

void checkClient(dataContainer* d, struct epoll_event* events, int iter , clientParameters* cd)
{
    struct timespec ts = {0};
    if( events[iter].events & EPOLLRDHUP )   
    {
        d->numOfClients--;
        d->numOfBlocks -= cd->numOfRequestedBlocks;
        char* buff = NULL;
        if(cd->numOfRequestedBlocks > 0)
            buff = calloc(cd->numOfRequestedBlocks * DATABLOCK, sizeof(char));
        if(read(d->toRead, buff, sizeof(buff)) == -1)
            errExit("read");
        if( epoll_ctl(d->epollfd, EPOLL_CTL_DEL, cd->fd, NULL))
            errExit("epoll_ctl 1");

        if(clock_gettime(CLOCK_REALTIME, &ts)== -1)
            errExit("clock_gettime");

        fprintf(stderr, "Client disconnected; TS: %ld.%ld address: %s port %d data lost %d\n", ts.tv_sec, ts.tv_nsec,
            inet_ntoa( cd->clientAddr.sin_addr ), ntohs( cd->clientAddr.sin_port ), cd->numOfRequestedBlocks * DATABLOCK);
        if(buff != NULL)
            free(buff);
        close(cd->fd);
        free(cd);
    }

    else if( events[iter].events & EPOLLOUT )
        operateOnClient( events, iter, d);
                  
}

int operateOnClient( struct epoll_event* events, int iter, dataContainer* d )
{
    char buff[DATABLOCK] = {0};
    clientParameters* cd = events[iter].data.ptr;
    
    if(read(d->toRead, buff, sizeof(buff)) == -1)
        errExit("read");
     
    if( write(cd->fd, buff, sizeof(buff) ) == -1)
        errExit("write");    
        
    cd->numOfRequestedBlocks--;
    d->numOfBlocks--;
    if(cd->numOfRequestedBlocks == 0)
        disconnectFromServer(cd, d);
    else
    {
        struct epoll_event ev = {0};
        ev.events = EPOLLOUT | EPOLLRDHUP;
        ev.data.ptr = cd;
        if ( epoll_ctl( d->epollfd, EPOLL_CTL_MOD, cd->fd, &ev ) == -1 ) 
            errExit("epoll_ctl ");  
    }
    
    return 0;
}

void addClientToEpoll(dataContainer* d, int flags, int fdToAdd, int checkAddr, struct sockaddr_in addr)
{
    clientParameters* cd = calloc(1, sizeof(clientParameters));
    cd->fd = fdToAdd;
    cd->numOfRequestedBlocks = 4; 
    if(checkAddr == 1) cd->clientAddr = addr;
    d->ev.events = flags;
    d->ev.data.ptr = cd;
    if (epoll_ctl(d->epollfd, EPOLL_CTL_ADD, fdToAdd, &(d->ev) ) == -1) 
        errExit("epoll_ctl ");

}

void placeClientInRingBuffOrEpoll(dataContainer* d, int client_sock)
{
    d->numOfClients++;
    int str;
    if( ioctl(d->toRead, FIONREAD, &str) == -1)
        errExit("ioctl");
        
    if(str <( (d->numOfBlocks * DATABLOCK)  + DATAPORTION))
        addElem(d, client_sock);
    else
    {   
        int sockfd;
        if(d->size > 0)
        {
            addElem(d, client_sock);
            sockfd = removeFirstElem(d);
        }
        else sockfd = client_sock;
        struct sockaddr_in addr = {0};
        socklen_t addLen = sizeof(addr);
        if(getpeername(sockfd, (struct sockaddr* )&addr, &addLen) == -1)
            errExit("getpeername");
        addClientToEpoll(d, (EPOLLOUT | EPOLLRDHUP) , sockfd, 1, addr );
        d->numOfBlocks +=4;
    }
}

void createSetEpoll(dataContainer* d)
{
    if((d->epollfd = epoll_create1(0)) == -1)
        errExit("epoll_create1");
    
    struct sockaddr_in addr = {0};
    addClientToEpoll(d, EPOLLIN, d->server_fd, 0, addr );
    addClientToEpoll(d, EPOLLIN, d->timerfd, 0, addr);    
    d->ringBuffer = calloc(MAXCLIENTS, sizeof(int));
}

void armTimer(dataContainer* d)
{
    struct itimerspec value;

    value.it_value.tv_sec = 5;
    value.it_value.tv_nsec = 0;

    value.it_interval.tv_sec = 5;
    value.it_interval.tv_nsec = 0;

    d->timerfd = timerfd_create(CLOCK_REALTIME, 0);    
    if (d->timerfd == -1)        
        errExit("timerfd_create");    

    if (timerfd_settime(d->timerfd, 0, &value, NULL) == -1)       
        errExit("timerfd_settime");
}

void disconnectFromServer(clientParameters* cd, dataContainer* d)
{
    d->numOfClients--;
    shutdown( cd->fd, SHUT_RDWR );
    struct timespec ts = {0};
    if ( clock_gettime( CLOCK_REALTIME, &ts) == -1 ) 
        errExit("clock_gettime");
    if ( epoll_ctl( d->epollfd, EPOLL_CTL_DEL, cd->fd, NULL) == -1 )
        errExit("epoll_ctl ");

    fprintf(stderr, "TS: %ld.%ld; address: %s port %d lost packages: %d \n",ts.tv_sec, ts.tv_nsec,
    inet_ntoa( cd->clientAddr.sin_addr ), ntohs( cd->clientAddr.sin_port ), cd->numOfRequestedBlocks);
    free(cd);
}

void child(int toWrite, dataContainer* d)
{
    int pipeCapacity = fcntl(toWrite, F_GETPIPE_SZ);
    int storagedInPipe = 0;
    int onProgress = 1; 
    struct timespec ts ={0};
    float times = BLOCK / (RATE * d->frequency);
    ts.tv_sec = (long)times;
    ts.tv_nsec = (long)((times - ts.tv_sec )*1e9);
    char c = 'a';
    
    while(onProgress)
    {
        if( ioctl(toWrite, FIONREAD, &storagedInPipe) == -1)
            errExit("ioctl");
        while(pipeCapacity - storagedInPipe >= BLOCK)
        {
            if( insertBlock(toWrite, c) == -1)
            {
                onProgress = 0;
                break;
            }
            c+= 1;
            if(c > 'z')
                c = 'A';
            if(c > 'Z')
                c = 'a';
                
            if(nanosleep(&ts, NULL) == -1)
                errExit("nanosleep");
            
        
            if( ioctl(toWrite, FIONREAD, &storagedInPipe) == -1)
                errExit("ioctl"); 
        }
    } 
   
}

int insertBlock(int fdToWrite, char c)
{
    char producedData[BLOCK]={0}; 
    int errorOccured = 0;

    for(int i=0; i< BLOCK - 1; i++)
        producedData[i] = c;
    producedData[BLOCK-1]= '\0';

    if(write(fdToWrite, producedData, BLOCK)== -1)
        if(errno == EPIPE)
            errorOccured = -1;

    return errorOccured;
}

void parseArguments(int argc, char** argv, dataContainer* d)
{
  int opt;
  while( (opt=getopt(argc, argv, "p:")) != -1 )
  {
    switch(opt)
    {
      case 'p':
            d->frequency = parseFloat(optarg);
            break;
     
      default:
            printf("Wrong parameters!\n");
            exit(EXIT_FAILURE);
    }
  }
}

float parseFloat(char* arr) 
{
    errno = 0;
    char* eptr;
    float val = strtof(arr, &eptr);
    if(*eptr != '\0'|| errno == ERANGE ) 
        errExit("strtof");
    return val;
}

void parseAddress(char* arg, dataContainer* d)
{
    char* arr1;
    char* arr2;
    arr1 = strtok( arg, "[]:" );
    if(arr1 == NULL)
        errExit("strtok 1");    

    arr2 = strtok( NULL, "[]:" );
    if(arr2 != NULL)
    {
        d->address = arr1;
        d->port = parseInt(arr2);
        if(strcpy(d->address, "localhost")) //it doesn't work on "localhost"
            d->address = "127.0.0.1";    //actually it works but inet_pton returns 0 and addr is set to INNER_ANY or sth
    }
    else
    {
        d->address = "127.0.0.1";    
        d->port = parseInt(arr1);          
    }
    short test = (short)d->port;
    if(test != d->port)
    {
        printf("port value is to big\n");
        exit(EXIT_FAILURE);
    }
}

int parseInt(char* arr )
{
    char *eptr;
    long val = strtol(arr, &eptr, 10);
	if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
		    || (errno != 0 && val == 0))
	{ errExit("strtol");}

    return val;
}

//for ring buffer
void addElem(dataContainer* b, int elem)
{
    if(b->size == MAXCLIENTS)
    {
        printf("ring buffer is full\n");
        exit(EXIT_FAILURE);
    }
    if( b->ringBuffer[b->lastUsed] == 0)
    {
        b->ringBuffer[b->lastUsed] = elem;
        if(b->lastUsed+1< MAXCLIENTS)
            b->lastUsed++;
        else b->lastUsed =0;
        b->size++;
    }
   
}
int removeFirstElem(dataContainer* b)
{
    int elem =  b->ringBuffer[b->firstUsed];
    b->ringBuffer[b->firstUsed]=0;
    if(b->firstUsed+1<MAXCLIENTS)
       b->firstUsed++;
    else b->firstUsed=0;
    b->size--;
    return elem;
}

int draw(dataContainer b, int e)
{
    if(e<MAXCLIENTS)
        return b.ringBuffer[e];
    return -1;
}