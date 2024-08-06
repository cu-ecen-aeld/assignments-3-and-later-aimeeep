#include <sys/types.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h> // exit
#include <netinet/in.h>
#include <string.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include "queue.h"

#define port_num 9000
#define FILE_PATH "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1025

typedef struct node{
    pthread_t tid;
    bool completed;
    int  socket_client;
    FILE* server_fd;
    char client_ip[INET6_ADDRSTRLEN];
} node_t;

// list start
typedef struct list_entry{
    node_t* node_data;
    SLIST_ENTRY(list_entry) next;
} list_entry_t;

SLIST_HEAD(slisthead, list_entry);
int socket_server;
int socket_client;
static FILE* server_fd = NULL;
static pthread_mutex_t file_mutex;
static struct slisthead head;
static pthread_t timestamp_thread_id;

void *timestamp_thread(void *arg) {
    time_t rawtime;
    struct tm *timeinfo;
    char time_str[100];
    while (true) {
        sleep(10);
        pthread_mutex_lock(&file_mutex);
        time(&rawtime);
        timeinfo = localtime(&rawtime);
        strftime(time_str, sizeof(time_str), "timestamp:%Y-%m-%d %H:%M:%S\n", timeinfo);
        server_fd = fopen(FILE_PATH,"a");
        if(fwrite(time_str, sizeof(char), strlen(time_str), server_fd)){
            perror("write");
        }
 	fclose(server_fd);
        pthread_mutex_unlock(&file_mutex);
    }
}

void client_thread_cleanup(node_t* thread_data){

    if (thread_data->socket_client > 0 ){
        close(thread_data->socket_client);
        syslog(LOG_DEBUG, "File closed by #%lu", thread_data->tid);
    }

    pthread_mutex_unlock(&file_mutex);
    syslog(LOG_DEBUG, "Mutex released by #%lu", thread_data->tid);

    close(thread_data->socket_client);
    syslog(LOG_DEBUG, "Closed connection from %s", thread_data->client_ip);

    //set thread as completed 
    thread_data->completed = true;
}

static void* client_thread(void* thread_args){
 
    node_t* node_data = (node_t*) thread_args;
    syslog(LOG_DEBUG, "Started new client thread #%lu for %s", node_data->tid, node_data->client_ip);
    char buffer[BUFFER_SIZE];        
    memset(&buffer[0], '\0', sizeof(buffer));

    pthread_mutex_lock(&file_mutex);
    syslog(LOG_DEBUG, "Mutex aquired by #%lu", node_data->tid);
    node_data->server_fd = fopen(FILE_PATH,"a");

    if (node_data->server_fd == NULL){
        syslog(LOG_DEBUG, "File cannot be opened, error: %s",strerror(errno));
        client_thread_cleanup(node_data);
        pthread_exit(node_data);
    }
    syslog(LOG_DEBUG, "File opened for appending by #%lu", node_data->tid);

    //receive message and dump to a file
    bool end = false;
    int byte_num;
    while(!end)
    {
        byte_num = recv(node_data->socket_client, buffer, BUFFER_SIZE - 1, 0);
        char received[BUFFER_SIZE];
        strcpy(received, buffer); // remove extra null characters
        syslog(LOG_DEBUG, "Received %d bytes: %s", byte_num, received);
    
        int ret = fwrite(received, sizeof(char), strlen(received), node_data->server_fd);
        syslog(LOG_DEBUG, "Wrote %d bytes: %s", ret, received);
        if (ret < 0){
            syslog(LOG_DEBUG, "Error: %s", strerror(errno));
            client_thread_cleanup(node_data);
            pthread_exit(node_data);
        }    
        if (strstr(received, "\n")){
    	    end = true;
    	    syslog(LOG_DEBUG, "Package end received");
        }
    	memset(&received[0], '\0', sizeof(received));
    	memset(&buffer[0], '\0', sizeof(buffer));
    }
    fclose(node_data->server_fd);
    end = false;
    node_data->server_fd = fopen(FILE_PATH,"r");
    while (!end) {
        byte_num = fread(buffer, sizeof(char), sizeof(buffer), node_data->server_fd);
        syslog(LOG_DEBUG, "Sending to socket client %d bytes %s", byte_num, buffer);
        if (byte_num >0 ){
            int ret = send(node_data->socket_client,buffer,byte_num,0);
            syslog(LOG_DEBUG, "Sending %d bytes: %s", byte_num, buffer);
            if(ret < 0){                      
		end = true;
            }
            memset(&buffer[0], '\0', sizeof(buffer));      
        }else{
        	end = true;
        }          
    } 
    client_thread_cleanup(node_data);
    node_data->completed = true;
    pthread_exit(node_data);
}

void node_list_cleanup(bool completed_only){

    syslog(LOG_DEBUG, "Check list");
    list_entry_t* elem;
    list_entry_t* telem;
    int ret;

    SLIST_FOREACH_SAFE(elem, &head, next, telem) {
        syslog(LOG_DEBUG, "Thread %lu completed: %d, joining...", elem->node_data->tid, elem->node_data->completed);
        // if completed join and free
        if ( elem->node_data->completed || !completed_only ){
            ret = pthread_join(elem->node_data->tid, NULL);
            if ( ret == 0){
                syslog(LOG_DEBUG, "Thread %lu joined, cleaning memory", elem->node_data->tid);
                SLIST_REMOVE(&head, elem, list_entry, next);
                free(elem->node_data);
                free(elem);
            }else{
                syslog(LOG_DEBUG, "Thread %lu join fail", elem->node_data->tid);
            }
        }
    }
}

static void graceful_stop(){
    syslog(LOG_DEBUG, "Caught signal, exiting");
    if (socket_server > 0){
        close(socket_server);
        syslog(LOG_DEBUG, "Closing server socket");
    }

    //join all thread
    node_list_cleanup(false);

    //join timestamp thread
    int ret = pthread_cancel(timestamp_thread_id);
    if ( ret != 0 ){
        //if fail to cancel try to wait
        sleep(1);
        pthread_cancel(timestamp_thread_id);
    }
    pthread_join(timestamp_thread_id, NULL);

    pthread_mutex_destroy(&file_mutex);
    closelog();
    exit(EXIT_SUCCESS);
}

static void daemonize()
{
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

static void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char** argv)
{
    	int ret;
    	openlog("aesdsocket.log", LOG_PID, LOG_USER);
    	syslog(LOG_DEBUG, "Starting aesdserver");	
	signal(SIGINT, graceful_stop);
    	signal(SIGTERM, graceful_stop);
    
	if( (socket_server = socket(PF_INET,SOCK_STREAM,0)) < 0) 
	{
		syslog(LOG_DEBUG, "Socket failed. errno: %d", errno);	
		return -1;
	}
        
        // map to a server socket location
	struct sockaddr_in address_client;
	address_client.sin_family = AF_INET;
	address_client.sin_addr.s_addr = INADDR_ANY;
	address_client.sin_port=htons(port_num);
	memset(&(address_client.sin_zero), '\0', 8);
	
	setsockopt(socket_server, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)); 
	if(bind(socket_server,(struct sockaddr *)&address_client,sizeof(address_client)) < 0) 
	{
		syslog(LOG_DEBUG, "Bind failed. errno: %d", errno);
		return -1;
	}

	//check if program should be deamonized
	if(argc ==2)
	{
		if(strcmp(argv[1], "-d") == 0)
		{
		    syslog(LOG_DEBUG, "Turning into a deamon");
		    daemonize();
		}
	}	
	
	if(listen(socket_server,3) < 0) 
	{
		syslog(LOG_DEBUG, "Listen failed. errno: %d", errno);
		return -1;
	}
	
	server_fd = fopen(FILE_PATH,"w");
	if(server_fd == NULL) 
	{
		syslog(LOG_DEBUG, "File open failed. errno: %d", errno);	
		return -1;
	}
	fclose(server_fd);
 	        	
	struct sockaddr_storage client_storage;
	socklen_t client_storage_size = sizeof(client_storage);
	char client_ip[INET6_ADDRSTRLEN];
	
	unsigned long tnum = 0;        	
	ret = pthread_mutex_init(&file_mutex,NULL);
        if (ret != 0){
            syslog(LOG_DEBUG, "Unable to setup mutex");
        }
        // start timestamp thread
        pthread_create(&timestamp_thread_id,NULL, timestamp_thread,NULL);
	
	SLIST_INIT(&head);
	while(true) 
	{
        	syslog(LOG_DEBUG, "Waiting for new client");
        	node_list_cleanup(true);
		if( (socket_client = accept(socket_server, (struct sockaddr*)&client_storage,
		          &client_storage_size)) < 0) 
		{
			syslog(LOG_DEBUG, "Accept failed. errno: %d", errno);
			continue;
		}
		          
		inet_ntop(client_storage.ss_family,get_in_addr((struct sockaddr *)&client_storage), client_ip, sizeof client_ip);
        	syslog(LOG_DEBUG, "Accepted connection from %s", client_ip);

        node_t* ct_data = malloc(sizeof(node_t));
        ct_data->tid = tnum;
        tnum = tnum + 1;
        strcpy(ct_data->client_ip, client_ip);
        ct_data->socket_client = socket_client;
        ct_data->completed = false;
        // start client thread & add to the list
        ret = pthread_create(&ct_data->tid,NULL,&client_thread, ct_data);
        if ( ret != 0){
            free(ct_data);
            continue;
        }
        list_entry_t* new_elem = malloc(sizeof(list_entry_t));
        new_elem->node_data = ct_data;
        SLIST_INSERT_HEAD(&head, new_elem, next); 
                
	}
}
