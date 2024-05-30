#include <syslog.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

int main(int argc, char *argv[]){

    openlog("writer.c", LOG_PID, LOG_USER);
    if( argc != 3){
        syslog(LOG_ERR,"Illegal number of parameters");
        closelog();
        return 1;
    }
    
    // assume the directory is created by the caller
    int bytes = strlen(argv[2]);
    const char *file_path = argv[1];
    int fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if(write(fd, argv[2], bytes) != bytes){
        syslog(LOG_ERR,"Truncated write");
        closelog();
        return 1;
    }
    
    syslog(LOG_INFO, "Writing %s to %s", argv[2], argv[1]);
    closelog();
    return 0;

}

