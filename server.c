#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>    /* Internet domain header */
#include <arpa/inet.h>     /* only needed on my mac */
#include "entities.h"

#define MAX_NUMBER_OF_CONNECTIONS 1024
#define LISTEN_BACKLOG 16

int CLIENT_IN_BUF_SIZE = sizeof(Message) * 4;
int CLIENT_OUT_BUF_SIZE = sizeof(Message) * 4;

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
    Client **client_sockets = malloc(sizeof(Client) * MAX_NUMBER_OF_CONNECTIONS);
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
            FD_SET(client_sockets[i]->soc, &read_fds);
            FD_SET(client_sockets[i]->soc, &write_fds);
            if (max_fd < client_sockets[i]->soc){
                max_fd = client_sockets[i]->soc;
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

            int client_soc = accept(listen_soc, (struct sockaddr *)&client_addr, &client_len);
            if (client_soc == -1){
                perror("accept");
            } else {
                // Create new client struct
                Client* new_client = malloc(sizeof(Client));
                new_client->soc = client_soc;
                new_client->in_buf_size = 0;
                new_client->out_buf_size = 0;

                client_sockets[current_number_of_connections] = new_client;

                num_new_connections += 1;
            }
        }

        for (int i = 0; i < current_number_of_connections; i++){
            Client *client = client_sockets[i];
            if(FD_ISSET(client->soc, &read_fds)){
                // code to read messages from connections
                int r = read(client->soc, client->in_buf + client->in_buf_size, 
                    sizeof(Message) - client->in_buf_size);
                if (r == -1){
                    perror("read");
                } else{
                    client->in_buf_size += r;
                    if (client->in_buf_size == sizeof(Message)){
                        // If full message is recieved add it into the output buffers & clean in buffer

                        printf("Message recieved \n");

                        for (int j = 0; j < current_number_of_connections; j++){
                            Client *recieving_client = client_sockets[j];
                            memcpy(recieving_client->out_buf + recieving_client->out_buf_size, 
                                client->in_buf, client->in_buf_size); // maybe change letter to  something safer
                            recieving_client->out_buf_size += client->in_buf_size;
                        }                       
                        
                        client->in_buf_size = 0;
                    }
                }
            }
        }

        for (int i = 0; i < current_number_of_connections; i++){
            Client *client = client_sockets[i];
            if(FD_ISSET(client->soc, &write_fds)){
                // code to boardcast messages
                int w = write(client->soc, client->out_buf, client->out_buf_size);
                if (w == -1){
                    perror("write");
                } else{
                    // copy the remainder of the message to the beginning of the buffer
                    memmove(client->out_buf, client->out_buf + w, client->out_buf_size - w);
                    client->out_buf_size = client->out_buf_size - w;
                }

            }
        }

        current_number_of_connections += num_new_connections;
    }

    return 0;
}


// Utils

// void msg_enqueue(MessageQueue *queue, Message message){
//     MessageList *message_item = malloc(sizeof(MessageList));
//     message_item->message = message;

//     if (!queue->head){
//         queue->head = message_item;
//         queue->tail = message_item;
//     } else {
//         message_item->prev = queue->tail;
//         queue->tail->next = message_item;
//         queue->tail = message_item;
//     }
// }

// Message *msg_dequeue(MessageQueue *queue){
//     if (queue->head){
//         Message *message = queue->head->message;
//         MessageList *old_head = queue->head;
//         queue->head = queue->head->next;
//         free(old_head);
//         if (queue->head){
//             queue->head->prev = NULL;
//         }
//         return message;
//     } else {
//         fprintf(stderr, "Dequeue: queue is empty");
//     }
// }

