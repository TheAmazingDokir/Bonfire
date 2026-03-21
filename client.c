#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>    /* Internet domain header */
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "entities.h"
#include <termios.h>

struct termios term, original;
tcgetattr(0, &term);            // Get current settings
original = term;                // Save original settings

void handler() {
    disable_non_canonical_input();
    exit(0);
}

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

    char message_text[256];
    while(1) {
        printf("> ");
        fgets(message_text, sizeof(message_text), stdin);
        Message *message = malloc(sizeof(Message));
        strcpy(message->data, message_text);
        message->data_size = strlen(message_text);
        char buf[sizeof(Message)];
        memcpy(buf, message, sizeof(Message));

        int count = 0;
        while(count < sizeof(Message)){
            int w = write(soc, buf+count, sizeof(Message)-count);
            count += w;
        }

        count = 0;

        while(count < sizeof(Message)){
            int r = read(soc, buf+count, sizeof(Message)-count);
            count += r;
        }
        
        memcpy(message, buf, sizeof(Message));
        message->data[message->data_size] = '\0';
        printf("%s\n", message->data);
        free(message);
    }
    return 0;
}


// Utils

void enable_non_canonical_input(){
    term.c_lflag &= ~ICANON;        // Disable canonical mode
    term.c_cc[VMIN] = 1;            // Require 1 character to read
    term.c_cc[VTIME] = 0;           // No timeout
    tcsetattr(0, TCSANOW, &term);   // Apply new settings
}

void disable_non_canonical_input(){
    tcsetattr(0, TCSANOW, &original);
}