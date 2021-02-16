#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <limits.h>

#define errExit(msg)    do { perror(msg); exit(EXIT_FAILURE);} while (0)

#define MAGAZINE 	30720
#define DEGRADATION_TIME 819
#define CONSUMPTION_TIME 4435
#define DATAPORTION 13312
#define DATABLOCK 3328
#define TIMER_SIG SIGRTMAX


typedef struct
{
    struct timespec connectAndFirstPackage;
    struct timespec firstPorionAndEnd;
    struct sockaddr_in addr;

}timesToReport;

typedef struct datacontainer
{
    int capacity;
    float consumption;
    float degradation;
    int port;
    char* address;
    int socket;
    struct sockaddr_in server;
    timer_t timerId;
    int magazineCapacity;
    struct timespec ts;

}dataContainer;




int parseInt(char* arr );
float parseFloat(char* arr);
void parseArguments(int argc, char** argv, dataContainer* d);
void parseAddress(char* arg, dataContainer* d);
void createSocket(dataContainer* d);
void operateOnData(dataContainer* d);
void extFun(int status, void* arg);
void getData(dataContainer* d);

int main(int argc, char** argv)
{
    dataContainer d={0};
    parseArguments(argc,argv, &d);
    parseAddress(argv[argc-1], &d);
    operateOnData(&d);
    
	close(d.socket);

    return 0;
}

void parseArguments(int argc, char** argv, dataContainer* d)
{
  int opt;
  while( (opt=getopt(argc, argv, "p:d:c:")) != -1 )
  {
    switch(opt)
    {
      case 'p':
            d->consumption = parseFloat(optarg);
            break;
     case 'd':
            d->degradation = parseFloat(optarg);
            break;
     case 'c':
            d->capacity = parseInt(optarg);
            d->magazineCapacity = d->capacity * MAGAZINE;
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
        if(strcpy(d->address, "localhost"))
            d->address = "127.0.0.1";   
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

void createSocket(dataContainer* d)
{
    if ((d->socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) 
        errExit("socket");
	
	d->server.sin_family = AF_INET;
	d->server.sin_port = htons( d->port );
    if(inet_pton(AF_INET, d->address, &d->server.sin_addr)==0)  
        errExit("inet_pton");

    if (connect(d->socket , (struct sockaddr *)&d->server , sizeof(d->server)) < 0)
        errExit("connect");

    if(clock_gettime(CLOCK_REALTIME, &(d->ts) )== -1)
        errExit("clock_gettime");
}

void operateOnData(dataContainer* d)
{
    struct timespec ts = {0};
	while(d->magazineCapacity > DATAPORTION)
	{
	    createSocket(d);
        getData(d);
	}

    close(d->socket);

    if(clock_gettime(CLOCK_REALTIME, &ts)== -1)
        errExit("clock_gettime");

    fprintf( stderr, "End; TS: sec: %ld nanosec: %ld; \n",ts.tv_sec, ts.tv_nsec);
}

void getData(dataContainer* d)
{
    char server_reply[DATABLOCK];
    struct timespec first = {0};
    struct timespec last = {0};
    struct timespec ts2 = {0};
    struct timespec first2 = {0};


    double times = DATABLOCK / (CONSUMPTION_TIME * d->consumption);
    ts2.tv_sec = (long)times;
    ts2.tv_nsec = (long)((times - ts2.tv_sec )*1e9);

    
    for(int i=0; i< 4; i++)
    {
        if( recv(d->socket, server_reply, DATABLOCK, 0) == -1)
            errExit("recv");

        d->magazineCapacity -= DATABLOCK;

        if(nanosleep(&ts2, NULL) == -1)
            errExit("nanosleep");

        if(i == 0)
        {
            if(clock_gettime(CLOCK_MONOTONIC, &first)== -1)
                errExit("clock_gettime");
            
            if(clock_gettime(CLOCK_REALTIME, &first2)== -1)
                errExit("clock_gettime");
        }    

    }
    
     if(clock_gettime(CLOCK_MONOTONIC, &last)== -1)
                errExit("clock_gettime");

    timesToReport* ttR = calloc(1, sizeof(timesToReport));

    ttR->connectAndFirstPackage.tv_sec = first2.tv_sec - d->ts.tv_sec;
    ttR->connectAndFirstPackage.tv_nsec = first2.tv_nsec - d->ts.tv_nsec;

    ttR->firstPorionAndEnd.tv_sec = last.tv_sec - first.tv_sec;
    ttR->firstPorionAndEnd.tv_nsec = last.tv_nsec - first.tv_nsec;
        
    //just to remove minus in nsec 
    if(ttR->firstPorionAndEnd.tv_nsec < 0)
    {
        ttR->firstPorionAndEnd.tv_sec -=1;
        ttR->firstPorionAndEnd.tv_nsec = 1e9 + ttR->firstPorionAndEnd.tv_nsec;
    }

    if(ttR->connectAndFirstPackage.tv_nsec < 0)
    {
        ttR->connectAndFirstPackage.tv_sec -=1;
        ttR->connectAndFirstPackage.tv_nsec = 1e9 + ttR->connectAndFirstPackage.tv_nsec;
    }

    int deg =  (int)((double)ttR->firstPorionAndEnd.tv_sec + (double)(ttR->firstPorionAndEnd.tv_nsec/1e9));
    int deg2 = (int)((double)ttR->connectAndFirstPackage.tv_sec + (double)(ttR->connectAndFirstPackage.tv_nsec/1e9));   //time of waiting in ring buffer
    d->magazineCapacity = d->magazineCapacity +  (int)(deg * DEGRADATION_TIME * d->degradation) + (int)(deg2 * DEGRADATION_TIME * d->degradation) ;

    socklen_t s = sizeof(ttR->addr);
    if( getsockname(d->socket, (struct sockaddr* )&(ttR->addr), ( socklen_t* )&s) == -1)
        errExit("getsockname");

    on_exit(extFun, ttR);
}

void extFun(int status, void* arg)
{
    timesToReport* t = ( timesToReport* )arg;
    fprintf(stderr, "PID: %d address %s port %d\n",  getpid(), inet_ntoa( t->addr.sin_addr ), ntohs( t->addr.sin_port ) );
    fprintf(stderr, "Time between first package and disconnection: %ld.%ld\n",t->connectAndFirstPackage.tv_sec, t->connectAndFirstPackage.tv_nsec);
    fprintf(stderr, "Delay between first and last package: %ld.%ld\n", t->firstPorionAndEnd.tv_sec, t->firstPorionAndEnd.tv_nsec);
    printf("status: %d\n", status); //just to remove warnings from -Wall -Wextra -Wpedantic
}