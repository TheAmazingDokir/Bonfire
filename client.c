#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>    /* Internet domain header */
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>

int main() {
    // create socket
    int soc = socket(AF_INET, SOCK_STREAM, 0);
    if (soc == -1) {
        perror("client: socket");
        exit(1);
    }


    //initialize server address    
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(54321);  
    memset(&server.sin_zero, 0, 8);
    
    struct addrinfo *ai;
    
    // get hostname of current device
    char hostname[1024];
    gethostname(hostname, sizeof(hostname));

    /* this call declares memory and populates ailist */
    getaddrinfo(hostname, NULL, NULL, &ai);
    server.sin_addr = ((struct sockaddr_in *) ai->ai_addr)->sin_addr;


    // free the memory that was allocated by getaddrinfo for this list
    freeaddrinfo(ai);

    int ret = connect(soc, (struct sockaddr *)&server, sizeof(struct sockaddr_in));
    if (ret == -1) {
        printf("Server is not running...\n");
        printf("Try connecting later. \n");

        while(ret == -1){
            ret = connect(soc, (struct sockaddr *)&server, sizeof(struct sockaddr_in));
            sleep(1);
        }
    }

    printf("Connected to the server! \n");

    char message[10];
    while(1) {
        printf("Message: ");
        scanf("%s", message);
        message[7] = '\0';
        write(soc, message, 10);
    }
    return 0;
}