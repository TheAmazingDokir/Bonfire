#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>    /* Internet domain header */
#include <arpa/inet.h>     /* only needed on my mac */
#include "entities.h"

#define DEBUG 1  // 1 = debug on, 0 = off

#if DEBUG
    #define DEBUG_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
    #define DEBUG_PRINT(fmt, ...)
#endif


#define MAX_NUMBER_OF_CONNECTIONS 1024
#define LISTEN_BACKLOG 16

// func prototypes
void handle_new_connection(int listen_soc, Client **clients, int *count, int *num_new);
void handle_client_read(Client *client, Client **clients, int *count, int i);  
void handle_client_write(Client *client);
void archive_message(Message *msg);
void broadcast_message(Client **clients, int count, Client *sender, Message *msg);
void send_history(Client *target);

int CLIENT_IN_BUF_SIZE = sizeof(Message) * 51; // 2^14 bytes
int CLIENT_OUT_BUF_SIZE = sizeof(Message) * 51;

int main() {
    // create socket
    int listen_soc = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_soc == -1) {
        perror("server: socket");
        exit(1);
    }
    DEBUG_PRINT("[SERVER] Socket created: fd=%d\n", listen_soc);

    //initialize server address    
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(51984);  
    memset(&server.sin_zero, 0, 8);
    server.sin_addr.s_addr = INADDR_ANY;

    // bind socket to an address
    if (bind(listen_soc, (struct sockaddr *) &server, sizeof(struct sockaddr_in)) == -1) {
      perror("server: bind");
      close(listen_soc);
      exit(1);
    } 
    DEBUG_PRINT("[SERVER] Bound to port 51984\n");

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

    // Fix otherwise client sockets has garbage pointers that make you get 
    // segmentation faults over and over until you lose your mind trying to 
    // figure out where theyre coming from thinking its your new code
    // only to realize later that its just damned line 53 reading garbage
    memset(client_sockets, 0, sizeof(Client *) * MAX_NUMBER_OF_CONNECTIONS);

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
        if (FD_ISSET(listen_soc, &read_fds)){      
            handle_new_connection(listen_soc, client_sockets, &current_number_of_connections, &num_new_connections);
        }

        for (int i = 0; i < current_number_of_connections; i++){
            Client *client = client_sockets[i];
            if(FD_ISSET(client->soc, &read_fds)){
                handle_client_read(client, client_sockets, &current_number_of_connections, i);
            }
        }

        for (int i = 0; i < current_number_of_connections; i++){
            Client *client = client_sockets[i];
            if(FD_ISSET(client->soc, &write_fds)){
                handle_client_write(client);
            }
        }
        current_number_of_connections += num_new_connections;
    }

    return 0;
}

// Append one Message struct to chat.dat file
// params: msg - Message struct pointer
void archive_message(Message *msg) {
    FILE *f = fopen("chat.dat", "ab"); 

    if (f == NULL) {
        perror("fopen archive");
        return;
    }

    fwrite(msg, sizeof(Message), 1, f);
    fclose(f);
    DEBUG_PRINT("[ARCHIVE] Message saved to chat.dat (action=%s)\n", msg->action);
}

// Sends Message struct to specified clients
// params: 
//      clients - array of all connected Client pointers
//      count - number of clients in array
//      sender - pointer to Client who sent message
//      msg - pointer to Message struct
void broadcast_message(Client **clients, int count, Client *sender, Message *msg) {
    int sent_count = 0;
    for (int i = 0; i < count; i++) {

        // don't send again to sender
        if (clients[i]->soc == sender->soc) {
            continue;
        }

        // if out_buf has space then copy message into it
        if (clients[i]->out_buf_size + sizeof(Message) < CLIENT_OUT_BUF_SIZE) {
            memcpy(clients[i]->out_buf + clients[i]->out_buf_size, msg, sizeof(Message));
            clients[i]->out_buf_size += sizeof(Message);
            sent_count++;
        } else {
            DEBUG_PRINT("[WARN] Client fd=%d out_buf FULL (%d bytes). Message dropped.\n", clients[i]->soc, clients[i]->out_buf_size);
        }
    }
    DEBUG_PRINT("[BROADCAST] Sent to %d clients (excluded sender fd=%d)\n", sent_count, sender->soc);
}


// Sends all of the archived messages to a client
// params: target - the client to receive history
void send_history(Client *target) {
    FILE *f = fopen("chat.dat", "rb");
    if (f == NULL) {
        DEBUG_PRINT("[HISTORY] No chat.dat found, skipping history\n");
        return;
    }
    
    Message hist_msg;
    int count = 0;
    
    while (fread(&hist_msg, sizeof(Message), 1, f) == 1) {
        // mark message as history in case we want to do smthing fancy with chunked-history loading later like discord
        memset(hist_msg.action, 0, 8);
        strcpy(hist_msg.action, "HIST");
        
        // add it to target's out_buf
        if (target->out_buf_size + sizeof(Message) < CLIENT_OUT_BUF_SIZE) {
            memcpy(target->out_buf + target->out_buf_size, &hist_msg, sizeof(Message));
            target->out_buf_size += sizeof(Message);
            count++;
        } else {
            DEBUG_PRINT("[HISTORY] out_buf full, stopping at %d messages\n", count);
            break;
        }
    }
    
    fclose(f);
    DEBUG_PRINT("[HISTORY] Sent %d messages to client fd=%d\n", count, target->soc);
}



void handle_new_connection(int listen_soc, Client **clients, int *count, int *num_new) {
    struct sockaddr_in client_addr;
    unsigned int client_len = sizeof(struct sockaddr_in);
    client_addr.sin_family = AF_INET;

    int client_soc = accept(listen_soc, (struct sockaddr *)&client_addr, &client_len);
    if (client_soc == -1){
        perror("accept");
        return;
    }
    DEBUG_PRINT("[CONNECT] Client accepted: fd=%d, total=%d\n", client_soc, *count + 1);

    // Create new client struct
    Client* new_client = malloc(sizeof(Client));
    new_client->soc = client_soc;
    new_client->in_buf_size = 0;
    new_client->out_buf_size = 0;

    clients[*count] = new_client;
    (*num_new)++;

    DEBUG_PRINT("[CONNECT] Sending chat history to new client\n");
    send_history(new_client);
}

void handle_client_read(Client *client, Client **clients, int *count, int i) {

    // code to read messages from connections
    int r = read(client->soc, client->in_buf + client->in_buf_size, 
        sizeof(Message) - client->in_buf_size);
    
    if (r == -1) {
        perror("read");

    } else if (r == 0) {
        DEBUG_PRINT("[DISCONNECT] Client fd=%d | Was client %d of %d\n", client->soc, i, *count);
        printf("Client %d disconnected\n", client->soc);
        close(client->soc);
        free(client);

        // remove zombie client from array & shift remaining clients
        for (int k = i; k < *count - 1; k++) {
            clients[k] = clients[k + 1];
        }
        clients[*count - 1] = NULL;
        (*count)--; i--;

    } else {

        client->in_buf_size += r;
        if (client->in_buf_size == sizeof(Message)) {

            // If full message is recieved add it into the output buffers & clean in buffer
            Message *msg = (Message *)client->in_buf;
            DEBUG_PRINT("[RECV] Client fd=%d | Action=%s | DataSize=%d | Data=\"%.50s\"\n", client->soc, msg->action, msg->data_size, msg->data);
            
            if (strcmp(msg->action, "LOGIN") == 0) {
                strcpy(client->user_name, msg->user);
                DEBUG_PRINT("[LOGIN] Client fd=%d registered as '%s'\n", client->soc, msg->user);
                client->in_buf_size = 0;
                return;  // dont archive/broadcast the login
            }
            
            archive_message(msg); // saving message to files
            broadcast_message(clients, *count, client, msg);
            client->in_buf_size = 0;
        }
    }
}

void handle_client_write(Client *client) {
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

