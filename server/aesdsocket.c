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
#define BUFFER_SIZE 512

volatile sig_atomic_t sig_received = 0;
//volatile sig_atomic_t proc_run = 0;

// Mutex for synchronizing file writes
pthread_mutex_t file_mutex;

typedef struct node{
    pthread_t tid;
    bool completed;
    FILE*  client_fd;
    char client_ip[INET6_ADDRSTRLEN];
} node_t;
// list start
typedef struct list_entry{
    node_t* node_data;
    SLIST_ENTRY(list_entry) next;
} list_entry_t;
SLIST_HEAD(slisthead, list_entry);
static struct slisthead head;
// list end
static pthread_t timestamp_thread_id;

int socket_server;
int socket_client;
FILE* server_fd = NULL;

void *timestamp_thread(void *arg) {
    free(arg);
    server_fd = fopen(FILE_PATH,"r");
    if (server_fd == NULL) {
        perror("open");
        pthread_exit(NULL);
    }

    while (!sig_received) {
        sleep(10);

        time_t rawtime;
        struct tm *timeinfo;
        char time_str[100];

        time(&rawtime);
        timeinfo = localtime(&rawtime);
        strftime(time_str, sizeof(time_str), "tttimestamp:%Y-%m-%d %H:%M:%S\n", timeinfo);
        //syslog(LOG_DEBUG, "%s", time_str);	
        // Write timestamp to file
        //pthread_mutex_lock(&file_mutex);
        server_fd = fopen(FILE_PATH,"a");
        if(fwrite(time_str, sizeof(char), strlen(time_str), server_fd)){
            perror("write");
        }
        //pthread_mutex_unlock(&file_mutex);
    }

    fclose(server_fd);
    pthread_exit(NULL);
}

static void* client_thread(void* thread_args){
 
    node_t* node_data = (node_t*) thread_args;
    syslog(LOG_DEBUG, "Started new client thread #%lu for %s", node_data->tid, node_data->client_ip);
    char buffer[BUFFER_SIZE];
    memset(&buffer[0], '\0', sizeof(buffer));
    
    pthread_mutex_lock(&file_mutex);
    syslog(LOG_DEBUG, "Mutex aquired by #%lu", node_data->tid);
    server_fd = fopen(FILE_PATH,"a");
    node_data->client_fd = server_fd;
    if (server_fd < 0){
        syslog(LOG_DEBUG, "File cannot be opened, error: %s",strerror(errno));
        //client_thread_cleanup(thread_data);
        pthread_exit(node_data);
    }
    syslog(LOG_DEBUG, "File opened for appending by #%lu", node_data->tid);

    //receive message and dump to a file
    bool end = false;
    int byte_num;
    while(!end)
    {
        byte_num = recv(socket_client, buffer, BUFFER_SIZE - 1, 0);
        char received[BUFFER_SIZE];
        strcpy(received, buffer); // remove extra null characters
        syslog(LOG_DEBUG, "Received %d bytes: %s", byte_num, received);
    
        int ret = fwrite(received, sizeof(char), strlen(received), server_fd);
    
        syslog(LOG_DEBUG, "Wrote %d bytes: %s", ret, received);
    
        if (strstr(received, "\n")){
    	end = true;
    	syslog(LOG_DEBUG, "Package end received");
        }
    	memset(&received[0], '\0', sizeof(received));
    	memset(&buffer[0], '\0', sizeof(buffer));
    }
    fclose(server_fd);
    
    server_fd = fopen(FILE_PATH,"r");		
    end = false;
    memset(buffer,0,sizeof(buffer));
    while (!end) {
        byte_num = fread(buffer, sizeof(char), sizeof(buffer), server_fd);  
        syslog(LOG_DEBUG, "Read %d bytes", byte_num);
        if(byte_num > 0){
    	send(socket_client,buffer,byte_num,0);
    	syslog(LOG_DEBUG, "Sending %d bytes: %s", byte_num, buffer);
        }else {
    	end = true;
    	syslog(LOG_DEBUG, "File end sent");
        }
        memset(buffer,0,sizeof(buffer));
    } 
    fclose(server_fd);    

    //client_thread_cleanup(node_data);
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

void client_thread_cleanup(node_t* thread_data){

    if (thread_data->client_fd > 0 ){
        fclose(thread_data->client_fd);
        syslog(LOG_DEBUG, "File closed by #%lu", thread_data->tid);
    }

    pthread_mutex_unlock(&file_mutex);
    syslog(LOG_DEBUG, "Mutex released by #%lu", thread_data->tid);

    fclose(thread_data->client_fd);
    syslog(LOG_DEBUG, "Closed connection from %s", thread_data->client_ip);

    //set thread as completed 
    thread_data->completed = true;
}

static void graceful_stop(){
    syslog(LOG_DEBUG, "Caught signal, exiting");
    if (server_fd > 0){
        fclose(server_fd);
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

void handle_signal(int signal)
{
    if(signal == SIGINT || signal == SIGTERM)
    {
        syslog(LOG_INFO, "Caught signal, exiting");
        shutdown(socket_server, SHUT_RDWR);
        if (socket_server != -1) close(socket_server);
        //if (file_fd != -1) close(file_fd);
        shutdown(socket_client, SHUT_RDWR);
        if (socket_client != -1) close(socket_client);
        remove(FILE_PATH);
        graceful_stop();
    }
}

static void daemonize()
{
	 pid_t pid;
	 pid = fork(); // Fork off the parent process
	 if (pid < 0) {
	   exit(EXIT_FAILURE);
	 }
	 if (pid > 0) {
	   exit(EXIT_SUCCESS);
	 }

    	//add signal handlers again
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
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
	signal(SIGINT, handle_signal);
    	signal(SIGTERM, handle_signal);
    
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
	
	while(true) 
	{
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
        ct_data->client_fd = server_fd;
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


		
		char buffer[BUFFER_SIZE];
		memset(buffer, 0, sizeof(buffer));
		
		if (server_fd == NULL)
		{
			syslog(LOG_DEBUG, "File cannot be opened. errno: %d", errno);
			return -1;
		}




	}	
        close(socket_client);
        close(socket_server);  
        fclose(server_fd);
        closelog(); 
        return 0;
                   
}
