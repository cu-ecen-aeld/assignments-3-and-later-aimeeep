#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "queue.h"

#define BUFF_SIZE 1024
#define DATA_FILE "/var/tmp/aesdsocketdata"

typedef struct client_thread_data{
    pthread_t tid;
    int client_fd;
    int file_fd;
    char client_ip[INET6_ADDRSTRLEN];
    bool completed;
} client_thread_data;

typedef struct thread_entry{
    client_thread_data* thread_data;
    SLIST_ENTRY(thread_entry) next;
} thread_entry;

SLIST_HEAD(slisthead, thread_entry);

static int server_fd;
static pthread_mutex_t fd_lock; 
static struct slisthead head;
static pthread_t timestamp_thread_id;

void* timestamp_thread_func(void* thread_args){

    int fd;
    time_t t;
    struct tm* t_local;
    char time_str[200];
    char log_str[211];

    while(true){

        // set cancellation as deferred
        pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED,NULL);

        //sleep as specification
        sleep(10);

        // disable cancelation during lock period and file write
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

        pthread_mutex_lock(&fd_lock);
        fd = open(DATA_FILE,O_WRONLY|O_APPEND|O_CREAT, 0644);
        t = time(NULL);
        t_local = localtime(&t);
        strftime(time_str, sizeof(time_str), "%a, %d %b %Y %T %z", t_local);
        int len = sprintf(log_str, "timestamp:%s\n", time_str);
        write(fd, log_str, len);
        close(fd);
        pthread_mutex_unlock(&fd_lock);

        //enable cancellation in thread safe block
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    }
}

void thread_list_cleanup(bool completed_only){

    syslog(LOG_DEBUG, "Check thread list");

    thread_entry* elem;
    thread_entry* telem;
    int ret;
    
    SLIST_FOREACH_SAFE(elem, &head, next, telem) {
        syslog(LOG_DEBUG, "Thread %lu completed: %d, joining...", elem->thread_data->tid, elem->thread_data->completed);
        // if completed join and free
        if ( elem->thread_data->completed || !completed_only ){
            ret = pthread_join(elem->thread_data->tid, NULL);
            if ( ret == 0){
                syslog(LOG_DEBUG, "Thread %lu joined, cleaning memory", elem->thread_data->tid);
                SLIST_REMOVE(&head, elem, thread_entry, next);
                free(elem->thread_data);
                free(elem);
            }else{
                syslog(LOG_DEBUG, "Thread %lu join fail", elem->thread_data->tid);
            }
        }
    }
}

void client_thread_cleanup(client_thread_data* thread_data){

    if (thread_data->file_fd > 0 ){
        close(thread_data->file_fd);
        syslog(LOG_DEBUG, "File closed by #%lu", thread_data->tid);
    }

    pthread_mutex_unlock(&fd_lock);
    syslog(LOG_DEBUG, "Mutex released by #%lu", thread_data->tid);

    close(thread_data->client_fd);
    syslog(LOG_DEBUG, "Closed connection from %s", thread_data->client_ip);

    //set thread as completed 
    thread_data->completed = true;
}

static void* client_thread_func(void* thread_args){
    int ret;
    client_thread_data* thread_data = (client_thread_data*) thread_args;
    syslog(LOG_DEBUG, "Started new client thread #%lu for %s", thread_data->tid, thread_data->client_ip);

    //setup buffer
    char buffer[BUFF_SIZE];
    memset(buffer,0,sizeof(buffer));

    pthread_mutex_lock(&fd_lock);
    syslog(LOG_DEBUG, "Mutex aquired by #%lu", thread_data->tid);
    int tfile_fd = open(DATA_FILE,O_RDWR | O_APPEND | O_CREAT , 0644);
    thread_data->file_fd = tfile_fd;
    if (tfile_fd < 0){
        syslog(LOG_DEBUG, "File cannot be opened, error: %s",strerror(errno));
        client_thread_cleanup(thread_data);
        pthread_exit(thread_data);
    }
    syslog(LOG_DEBUG, "File opened for appending by #%lu", thread_data->tid);

    //receive message and dump to a file
    bool got_newline = false;
    ssize_t bytes_recv;
    while(!got_newline){
        
        bytes_recv = recv(thread_data->client_fd,buffer,sizeof(buffer),0);
        syslog(LOG_DEBUG, "Received %zd bytes", bytes_recv);

        ret = write(tfile_fd,buffer, bytes_recv);
        syslog(LOG_DEBUG, "Wrote %d bytes", ret);
        if (ret < 0){
            syslog(LOG_DEBUG, "Error: %s", strerror(errno));
            client_thread_cleanup(thread_data);
            pthread_exit(thread_data);
        }
        if(buffer[bytes_recv-1] == '\n')
            got_newline = true;
        memset(buffer,0,sizeof(buffer));

    }

    lseek(tfile_fd, 0, SEEK_SET);

    if (tfile_fd == 0){
        syslog(LOG_DEBUG, "File seek failed");
    }
    syslog(LOG_DEBUG, "File seek to start");

    int bytes_read;
    while ((bytes_read = read(tfile_fd,buffer, sizeof(buffer))) > 0) {

        ret = send(thread_data->client_fd,buffer,bytes_read,0);
        if (ret < 0 ){
            client_thread_cleanup(thread_data);
            thread_data->completed = true;
            pthread_exit(thread_data);
        }
        syslog(LOG_DEBUG, "Sent %d bytes",ret);

        memset(buffer,0,sizeof(buffer));
    }
    client_thread_cleanup(thread_data);
    pthread_exit(thread_data);
}

static void graceful_stop(int signum){
    syslog(LOG_DEBUG, "Caught signal, exiting");
    if (server_fd > 0){
        close(server_fd);
        syslog(LOG_DEBUG, "Closing server socket");
    }

    //join all thread
    thread_list_cleanup(false);

    //join timestamp thread
    int ret = pthread_cancel(timestamp_thread_id);
    if ( ret != 0 ){
        //if fail to cancel try to wait
        sleep(1);
        pthread_cancel(timestamp_thread_id);
    }
    pthread_join(timestamp_thread_id, NULL);

    pthread_mutex_destroy(&fd_lock);
    closelog();
    exit(EXIT_SUCCESS);
}

static void daemonize(){
    pid_t pid;

    pid = fork();

    //exit on error
    if(pid < 0){
        syslog(LOG_DEBUG, "Fork #1 failed");
        exit(EXIT_FAILURE);
    }
    syslog(LOG_DEBUG, "Fork #1 done");

    //stop parent
    if(pid > 0){
        syslog(LOG_DEBUG, "Stop parent #1");
        exit(EXIT_SUCCESS);
    }

    //change session id exit on failure
    if(setsid() < 0){
        syslog(LOG_DEBUG, "Set SID failed");
        exit(EXIT_FAILURE);
    }
    syslog(LOG_DEBUG, "Set SID done");

    //fork again to deamonize
    pid = fork();

    //exit on error
    if(pid < 0){
        syslog(LOG_DEBUG, "Fork #2 failed");
        exit(EXIT_FAILURE);
    }
    syslog(LOG_DEBUG, "Fork #2 done");

    //stop parent
    if(pid > 0){
        syslog(LOG_DEBUG, "Stop parent #2");
        exit(EXIT_SUCCESS);
    }

    //add signal handlers again
    signal(SIGINT, graceful_stop);
    signal(SIGTERM, graceful_stop);
    syslog(LOG_DEBUG, "Registered daemon signal handler");
}

// get sockaddr, IPv4 or IPv6:
static void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char **argv){

    setlogmask(LOG_UPTO (LOG_DEBUG));
    openlog("aesdsocket.log", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL0);
    syslog(LOG_DEBUG, "Starting aesdserver");


    signal(SIGINT, graceful_stop);
    signal(SIGTERM, graceful_stop);
    syslog(LOG_DEBUG, "Registered signal handler");

    int ret;
    struct addrinfo hints;
    struct addrinfo* servinfo;

    //socket configurations
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // don't care IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP stream sockets
    hints.ai_flags = AI_PASSIVE;     // fill in my IP for me
    
    //get socket info
    ret = getaddrinfo(NULL,"9000", &hints, &servinfo);
    if (ret < 0){
        syslog(LOG_DEBUG, "Unable to get address info");
        return -1;
    }

    //create socket file_fd
    server_fd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
    if (ret < 0){
        syslog(LOG_DEBUG, "Unable to create server socket");
        return -1;
    }
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)); 
    //bind socket to address
    ret = bind(server_fd, servinfo->ai_addr, servinfo->ai_addrlen);
    if (ret < 0){
        syslog(LOG_DEBUG, "Socket bind failed");
        return -1;
    }

    //check if program should be deamonized
    if(argc==2){
        if(strcmp(argv[1], "-d") == 0){
            syslog(LOG_DEBUG, "Turning into a deamon");
            daemonize();
        }
    }

    //free memory
    freeaddrinfo(servinfo);

    //start listening
    ret = listen(server_fd,1);
    if (ret < 0){
        syslog(LOG_DEBUG, "Unable to listen for new connection");
        return -1;
    }

    int file_fd = open(DATA_FILE,O_RDONLY | O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if (file_fd == 0){
        syslog(LOG_DEBUG, "File cannot be opened");
        return -1;
    }
    close(file_fd);
    syslog(LOG_DEBUG, "File cleared");

    //wait for client connection
    struct sockaddr_storage client_addr;
    socklen_t client_addr_size = sizeof(client_addr);
    char client_ip[INET6_ADDRSTRLEN];

    //setup thread stuff
    unsigned long tnum = 0;
    ret = pthread_mutex_init(&fd_lock,NULL);
    if (ret != 0){
        syslog(LOG_DEBUG, "Unable to setup mutex");
    }

    pthread_create(&timestamp_thread_id,NULL, timestamp_thread_func,NULL);

    //init thread list
    SLIST_INIT(&head);
    int client_fd;

    while (true) {

        // check list and join completed thread
        thread_list_cleanup(true);

        syslog(LOG_DEBUG, "Waiting for new client");
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_size);
        if (client_fd < 0){
            syslog(LOG_DEBUG, "Connection accept failed");
            continue;
        }
        
        inet_ntop(client_addr.ss_family,get_in_addr((struct sockaddr *)&client_addr),
            client_ip, sizeof client_ip);
        syslog(LOG_DEBUG, "Accepted connection from %s", client_ip);

        client_thread_data* ct_data = malloc(sizeof(client_thread_data));
        ct_data->tid = tnum;
        tnum = tnum + 1;
        strcpy(ct_data->client_ip, client_ip);
        ct_data->client_fd = client_fd;
        ct_data->completed = false;

        ret = pthread_create(&ct_data->tid,NULL,&client_thread_func, ct_data);
        if ( ret != 0){
            free(ct_data);
            continue;
        }
        // add thread to list
        thread_entry* new_elem = malloc(sizeof(thread_entry));
        new_elem->thread_data = ct_data;
        SLIST_INSERT_HEAD(&head, new_elem, next);

    }   
}
