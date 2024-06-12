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
#define port_num 9000
#define FILE_PATH "/var/tmp/aesdsocketdata"


int socket_server;
int socket_client;
FILE* server_fd = NULL;

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
        closelog();
        exit(EXIT_SUCCESS);
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
	
	server_fd = fopen("/var/tmp/aesdsocketdata","w");
	if(server_fd == NULL) 
	{
		syslog(LOG_DEBUG, "File open failed. errno: %d", errno);	
		return -1;
	}
	fclose(server_fd);
	
	struct sockaddr_storage client_storage;
	socklen_t client_storage_size = sizeof(client_storage);
	char client_ip[INET6_ADDRSTRLEN];
	
	while(true) 
	{
		if( (socket_client = accept(socket_server, (struct sockaddr*)&client_storage,
		          &client_storage_size)) < 0) 
		{
			syslog(LOG_DEBUG, "Accept failed. errno: %d", errno);
			return -1;
		}
		          
		inet_ntop(client_storage.ss_family,get_in_addr((struct sockaddr *)&client_storage), client_ip, sizeof client_ip);
        	syslog(LOG_DEBUG, "Accepted connection from %s", client_ip);
		
		char buffer[512];
		memset(buffer, 0, sizeof(buffer));
		server_fd = fopen("/var/tmp/aesdsocketdata","a");
		if (server_fd == NULL)
		{
			syslog(LOG_DEBUG, "File cannot be opened. errno: %d", errno);
			return -1;
		}
		bool end = false;
		int byte_num;
		while(!end)
		{
		    byte_num = recv(socket_client, buffer, sizeof(buffer), 0);
		    syslog(LOG_DEBUG, "Received %d bytes: %s", byte_num, buffer);

		    int ret = fwrite(buffer, sizeof(char), strlen(buffer), server_fd);
		    memset(buffer,0,sizeof(buffer));
		    syslog(LOG_DEBUG, "Wrote %d bytes: %s", ret, buffer);

		    if (strcmp(&buffer[byte_num], "\n") && byte_num != 512){
		        end = true;
		        syslog(LOG_DEBUG, "Package end received");
		    }
		}
		fclose(server_fd);
		
		server_fd = fopen("/var/tmp/aesdsocketdata","r");		
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
	}	
        close(socket_client);
        close(socket_server);  
        fclose(server_fd);
        closelog(); 
        return 0;
                   
}
