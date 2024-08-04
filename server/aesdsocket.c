#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <syslog.h>
#include <errno.h>
#include <sys/select.h>
#include <time.h>
#include <pthread.h>
#include "queue.h"

#define PORT "9000"
//#define BACKLOG 10
#define FILE_PATH "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024
#define TIMEOUT_SEC 1

volatile sig_atomic_t sig_received = 0;
volatile sig_atomic_t proc_run = 0;
int server_socket;
int *client_socket;

// Mutex for synchronizing file writes
pthread_mutex_t file_mutex;

// Linked list structure for managing threads
typedef struct node {
    pthread_t thread_id;
    int client_socket;
    TAILQ_ENTRY(node) nodes;
} node_t;

typedef TAILQ_HEAD(head_s, node) head_t;
pthread_mutex_t list_mutex;
head_t head;

void handle_signal() {
//    head_t *list;
    syslog(LOG_INFO, "Caught signal, exiting");
    sig_received = 1;
/*    if (!proc_run){
    	struct node *e = NULL;
    	TAILQ_FOREACH(e, list, nodes){
    	  if (e->client_socket != -1) close(e->client_socket);
    	}
    	_free_queue(list);
	if (server_socket != -1) close(server_socket);
	if (file_fd != -1) close(file_fd);
	remove(FILE_PATH);
	closelog();
	exit(0);
    }*/
}

void daemonize() {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        perror("setsid");
        exit(EXIT_FAILURE);
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }
}

void *process_client(void *arg) {
    int client_socket = *(int*)arg;
    free(arg);
    char buffer[BUFFER_SIZE];
    ssize_t num_bytes;
    char *packet = NULL;
    size_t packet_len = 0;
    char *newline_pos;
    proc_run = 1;
    
    pthread_mutex_lock(&file_mutex);
    int file_fd = open(FILE_PATH, O_RDWR | O_APPEND | O_CREAT, 0644);
    if (file_fd == -1) {
        perror("open");
        close(client_socket);
        pthread_mutex_unlock(&file_mutex);
        pthread_exit(NULL);
    }
    
    while(!sig_received){
    	// receiving data
		num_bytes = recv(client_socket, buffer, BUFFER_SIZE, 0);
		// check if receiving is good
		if(num_bytes < 0){
			perror("recv");
			if(packet) free(packet);
			close(file_fd);
			pthread_mutex_unlock(&file_mutex);
			pthread_exit(NULL);
		}
		//add received data to the packet
		packet = realloc(packet, packet_len + num_bytes);
		if (!packet) {
			syslog(LOG_ERR, "Error reallocating memory: %s", strerror(errno));
			close(file_fd);
			pthread_mutex_unlock(&file_mutex);
			pthread_exit(NULL);
		}
		memcpy(packet + packet_len, buffer, num_bytes);
		packet_len += num_bytes;
//	    printf("packet received: %ld bytes\n", packet_len);
//	    printf("last packet value: %d\n", packet[packet_len-1]);
	    
		// check for newline and save
		if (num_bytes < BUFFER_SIZE) {
			//check newline position
			newline_pos = strchr(packet, '\n');
			if (newline_pos){
				// writing data into the file
				ssize_t bytes_written = write(file_fd, packet, packet_len);
				if (bytes_written == -1) {
					syslog(LOG_ERR, "Error writing to file: %s", strerror(errno));
					free(packet);
					close(file_fd);
					pthread_mutex_unlock(&file_mutex);
					pthread_exit(NULL);
				}
//				printf("bytes written: %ld\n", bytes_written);
//				printf("packet value: %s\n", packet);

				// Read back the file and send its contents to the client
				lseek(file_fd, 0, SEEK_SET);
				off_t file_size = lseek(file_fd, 0, SEEK_END);
				lseek(file_fd, 0, SEEK_SET);
				off_t offset = 0;
				while (offset < file_size) {
					num_bytes = pread(file_fd, buffer, BUFFER_SIZE, offset);
					if (num_bytes == -1) {
						printf("error reading from file: %s\n", strerror(errno));
						syslog(LOG_ERR, "Error reading from file: %s", strerror(errno));
						free(packet);
						close(file_fd);
						pthread_mutex_unlock(&file_mutex);
						pthread_exit(NULL);
					}
					printf("buffer value: %s\n", buffer);
					if (send(client_socket, buffer, num_bytes, 0) == -1) {
						syslog(LOG_ERR, "Error sending data: %s", strerror(errno));
						free(packet);
						close(file_fd);
						pthread_mutex_unlock(&file_mutex);
						pthread_exit(NULL);
					}
					offset += num_bytes;
				}
				free(packet);
				close(file_fd);
				pthread_mutex_unlock(&file_mutex);
				pthread_exit(NULL);
			}
		}
    }
    if(packet) free(packet);
    close(file_fd);
    pthread_mutex_lock(&file_mutex);
    pthread_exit(NULL);
}

void add_thread_to_list(pthread_t thread_id, int client_socket, head_t *head) {
    pthread_mutex_lock(&list_mutex);
    struct node *new_node = malloc(sizeof(struct node));
    new_node->thread_id = thread_id;
    new_node->client_socket = client_socket;
    TAILQ_INSERT_TAIL(head, new_node, nodes);
    pthread_mutex_unlock(&list_mutex);
}

void cleanup_threads(head_t *head) {
    pthread_mutex_lock(&list_mutex);
    struct node *current = NULL;
    struct node *temp = NULL;
    TAILQ_FOREACH_SAFE(current, head, nodes, temp) {
        pthread_join(current->thread_id, NULL);
        close(current->client_socket);
        syslog(LOG_INFO, "Closed connection");
        TAILQ_REMOVE(head, current, nodes);
        free(current);
        current = NULL;
    }
    pthread_mutex_unlock(&list_mutex);
}

void *timestamp_thread(void *arg) {
    free(arg);
    int file_fd = open("/var/tmp/aesdsocketdata", O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (file_fd == -1) {
        perror("open");
        pthread_exit(NULL);
    }

    while (!sig_received) {
        sleep(10);

        // Get current time
        time_t rawtime;
        struct tm *timeinfo;
        char time_str[100];

        time(&rawtime);
        timeinfo = localtime(&rawtime);
        strftime(time_str, sizeof(time_str), "timestamp:%Y-%m-%d %H:%M:%S\n", timeinfo);

        // Write timestamp to file
        pthread_mutex_lock(&file_mutex);
        if (write(file_fd, time_str, strlen(time_str)) != (ssize_t)strlen(time_str)) {
            perror("write");
        }
        pthread_mutex_unlock(&file_mutex);
    }

    close(file_fd);
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    int status;
    struct addrinfo hints, *res, *p;
    struct sockaddr_storage client_addr;
    socklen_t addr_size;
    char client_ip[INET6_ADDRSTRLEN];
    int opt = 1;
    
    // Initialize mutexes
    pthread_mutex_init(&file_mutex, NULL);
    pthread_mutex_init(&list_mutex, NULL);

    // Initialize the thread list
    TAILQ_INIT(&head);

    openlog("aesdsocket", LOG_PID, LOG_USER);

//    signal(SIGINT, handle_signal);
//    signal(SIGTERM, handle_signal);
// Setup signal handling
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;  // Allow IPv4
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;  // Use my IP
//    hints.ai_addr = INADDR_ANY;
    hints.ai_protocol = 0;
    
    if ((status = getaddrinfo(NULL, PORT, &hints, &res)) != 0) {
        syslog(LOG_ERR, "getaddrinfo: %s", gai_strerror(status));
        return -1;
    }
    
    // Create server socket and bind
    for (p = res; p != NULL; p = p->ai_next) {
        //create socket
        server_socket = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (server_socket == -1) {
            syslog(LOG_ERR, "Error creating socket: %s", strerror(errno));
            freeaddrinfo(res);
            continue;
        }
        //add socket's options
        if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
            perror("setsockopt");
            close(server_socket);
            freeaddrinfo(res);
            return -1;
        }

        //assign address to the socket
        if (bind(server_socket, p->ai_addr, p->ai_addrlen) == -1) {
            syslog(LOG_ERR, "Error binding socket: %s", strerror(errno));
            close(server_socket);
            freeaddrinfo(res);
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    
    if (p == NULL) {
        syslog(LOG_ERR, "Failed to bind socket\n");
        return -1;
    }
    
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemonize();
    }
        //listen for a connection: put the socket in passive mode to accept connections
	if (listen(server_socket, SOMAXCONN) == -1) {
	    syslog(LOG_ERR, "Error listening on socket: %s", strerror(errno));
	    close(server_socket);
	    return -1;
	}
	
	syslog(LOG_INFO, "Server is listening on port 9000...\n");
	
	// Create timestamp thread
    pthread_t timestamp_thread_id;
    if (pthread_create(&timestamp_thread_id, NULL, timestamp_thread, NULL) != 0) {
        perror("pthread_create");
        close(server_socket);
        return -1;
    }
        
	while (!sig_received){
		addr_size = sizeof(client_addr);
		//extracts the  first  connection  request  on  the  queue  of pending
		//connections for the listening socket
		//creates a new connected socket, and returns a new file descriptor 
		//referring to that socket.
		client_socket = malloc(sizeof(int));
		*client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &addr_size);
		if (*client_socket == -1) {
		    if(sig_received) break;
		    syslog(LOG_ERR, "Error accepting connection: %s", strerror(errno));
		    continue;
		}

		if (client_addr.ss_family == AF_INET) {
		    struct sockaddr_in *s = (struct sockaddr_in *)&client_addr;
		    inet_ntop(AF_INET, &s->sin_addr, client_ip, sizeof(client_ip));
		}else{
		    struct sockaddr_in6 *s = (struct sockaddr_in6 *)&client_addr;
		    inet_ntop(AF_INET6, &s->sin6_addr, client_ip, sizeof(client_ip));
		}

		syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        pthread_t thread_id;
        if(pthread_create(&thread_id, NULL, process_client, client_socket) != 0) {
            syslog(LOG_ERR, "Failed to create thread.");
            free(client_socket);
            continue;
        }
		proc_run = 0;
		add_thread_to_list(thread_id, *client_socket, &head);
	}

    close(server_socket);
    cleanup_threads(&head);
    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&list_mutex);
    remove(FILE_PATH);
    closelog();    
    return 0;
}
