#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>    /* Internet domain header */
#include <arpa/inet.h>     /* only needed on my mac */

#define MAX_NUMBER_OF_CONNECTIONS 1024
#define LISTEN_BACKLOG 16

int main() {
    // create socket
    int listen_soc = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_soc == -1) {
        perror("server: socket");
        exit(1);
    }


    //initialize server address    
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(54321);  
    memset(&server.sin_zero, 0, 8);
    server.sin_addr.s_addr = INADDR_ANY;

    // bind socket to an address
    if (bind(listen_soc, (struct sockaddr *) &server, sizeof(struct sockaddr_in)) == -1) {
      perror("server: bind");
      close(listen_soc);
      exit(1);
    } 


    // Set up a queue in the kernel to hold pending connections.
    if (listen(listen_soc, LISTEN_BACKLOG) < 0) {
        // listen failed
        perror("listen");
        exit(1);
    }

    // Initilize sockets and file descriptors
    int *client_sockets = malloc(sizeof(int) * MAX_NUMBER_OF_CONNECTIONS);
    int current_number_of_connections = 0;
    fd_set read_fds;
    fd_set write_fds;
    int max_fd = listen_soc;


    // Event loop
    while(1) {
        // Clean fds sets
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);

        // set listen socket to be read fd
        FD_SET(listen_soc, &read_fds);

        // loop over every active client and set up read/write fds
        for (int i = 0; i < current_number_of_connections; i++){
            FD_SET(client_sockets[i], &read_fds);
            FD_SET(client_sockets[i], &write_fds);
            if (max_fd < client_sockets[i]){
                max_fd = client_sockets[i];
            }
        }

        // selects all waiting sockets
        if (select(max_fd + 1, &read_fds, &write_fds, NULL, NULL) == -1){
            perror("select");
            exit(1);
        }

        int num_new_connections = 0;

        // Accept new connections
        if(FD_ISSET(listen_soc, &read_fds)){
            struct sockaddr_in client_addr;
            unsigned int client_len = sizeof(struct sockaddr_in);
            client_addr.sin_family = AF_INET;
            int client_socket = accept(listen_soc, (struct sockaddr *)&client_addr, &client_len);
            if (client_socket == -1){
                perror("accept");
            } else {
                client_sockets[current_number_of_connections] = client_socket;
                num_new_connections += 1;
            }

        }

        for (int i = 0; i < current_number_of_connections; i++){
            if(FD_ISSET(client_sockets[i], &read_fds)){
                // code to read messages from connections
                char line[10];
                read(client_sockets[i], line, 10);
                /* before we can use line in a printf statement, ensure it is a string */
                line[9] = '\0';
                printf("I read %s\n", line);
            }
        }

        for (int i = 0; i < current_number_of_connections; i++){
            if(FD_ISSET(client_sockets[i], &write_fds)){
                // code to write messages from connections
                // write(client_sockets[i], "hello\r\n", 7);
            }
        }

        current_number_of_connections += num_new_connections;
    }

    return 0;
}

