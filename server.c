#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h> /* Internet domain header */
#include <arpa/inet.h>  /* only needed on my mac */
#include <dirent.h>     /* for directory operations */
#include <time.h>
#include "set_ops.h"
#include "constants.h"
#include "entities.h"
#include "channels.h"

#define DEBUG 1 // 1 = debug on, 0 = off

#if DEBUG
#define DEBUG_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...)
#endif
#define MAX_NUMBER_OF_CONNECTIONS 1024
#define LISTEN_BACKLOG 16

// func prototypes
void handle_new_connection(int listen_soc, Client **clients, int *count, int *num_new);
void handle_client_read(Client *client, Client **clients, int *count, int i, Channel **channels, int *num_channels, PrivateChannel **private_channels, int *num_private_channels);
void handle_client_write(Client *client);
void archive_message(Message *msg);
void broadcast_message(Client **clients, int count, Client *sender, Message *msg, Channel **channels, int num_channels, int exclude_sender);
void send_history(Client *target);
void respond_to_client_with_message(Client *client, char *action, int msg_size, char *msg_data);
void broadcast_system_message(Client *client_to_exclude, Client **clients, int count, char *action, char *msg_data, Channel **channels, int num_channels);
void send_disconnect_message(Client *client_to_exclude, Client **clients, int count, Channel **channels, int num_channels);
void send_online_users_message(Client *client, Client **clients, int count, Channel **channels, int num_channels, PrivateChannel **private_channels, int num_private_channels);
void handle_friend_request(Client *client, Client **clients, int count, char *friend_name, Channel **channels, int *num_channels, PrivateChannel **private_channels, int *num_private_channels);
int verify_username(Client *client, Client **clients, int count, char* username);

int CLIENT_IN_BUF_SIZE = sizeof(Message) * 51; // 2^14 bytes
int CLIENT_OUT_BUF_SIZE = sizeof(Message) * 51;

int main()
{
    // create socket
    int listen_soc = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_soc == -1)
    {
        perror("server: socket");
        exit(1);
    }
    DEBUG_PRINT("[SERVER] Socket created: fd=%d\n", listen_soc);

    // initialize server address
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(51984);
    memset(&server.sin_zero, 0, 8);
    server.sin_addr.s_addr = INADDR_ANY;

    // bind socket to an address
    if (bind(listen_soc, (struct sockaddr *)&server, sizeof(struct sockaddr_in)) == -1)
    {
        perror("server: bind");
        close(listen_soc);
        exit(1);
    }
    DEBUG_PRINT("[SERVER] Bound to port 51984\n");

    // Set up a queue in the kernel to hold pending connections.
    if (listen(listen_soc, LISTEN_BACKLOG) < 0)
    {
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

    // Temp data for channels
    Channel *channels[128];
    PrivateChannel *private_channels[128];

    int num_channels = 0;
    int num_private_channels = 0;

    // Load channels
    load_channels(channels, &num_channels);
    load_private_channels(private_channels, &num_private_channels, channels, num_channels);

    // Event loop
    while (1)
    {
        // Clean fds sets
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);

        // set listen socket to be read fd
        FD_SET(listen_soc, &read_fds);

        // loop over every active client and set up read/write fds
        for (int i = 0; i < current_number_of_connections; i++)
        {
            FD_SET(client_sockets[i]->soc, &read_fds);
            FD_SET(client_sockets[i]->soc, &write_fds);
            if (max_fd < client_sockets[i]->soc)
            {
                max_fd = client_sockets[i]->soc;
            }
        }

        // selects all waiting sockets
        if (select(max_fd + 1, &read_fds, &write_fds, NULL, NULL) == -1)
        {
            perror("select");
            exit(1);
        }

        int num_new_connections = 0;

        // Accept new connections
        if (FD_ISSET(listen_soc, &read_fds))
        {
            handle_new_connection(listen_soc, client_sockets, &current_number_of_connections, &num_new_connections);
        }

        // Read client messages
        for (int i = 0; i < current_number_of_connections; i++)
        {
            Client *client = client_sockets[i];
            if (FD_ISSET(client->soc, &read_fds))
            {
                handle_client_read(client, client_sockets, &current_number_of_connections, i, channels, &num_channels, private_channels, &num_private_channels);
            }
        }

        // Send messages to clients
        for (int i = 0; i < current_number_of_connections; i++)
        {
            Client *client = client_sockets[i];
            if (FD_ISSET(client->soc, &write_fds))
            {
                handle_client_write(client);
            }
        }
        current_number_of_connections += num_new_connections;
    }

    return 0;
}

// Append one Message struct to chat.dat file
// params: msg - Message struct pointer
void archive_message(Message *msg)
{
    char channel_file[strlen(CHANNEL_DIR) + 36];
    strcpy(channel_file, CHANNEL_DIR);
    channel_file[strlen(CHANNEL_DIR)] = '/';
    channel_file[strlen(CHANNEL_DIR) + 1] = '\0';
    strcat(channel_file, msg->channel);
    strcat(channel_file, ".dat");

    FILE *f = fopen(channel_file, "ab");

    if (f == NULL)
    {
        perror("fopen archive");
        return;
    }

    fwrite(msg, sizeof(Message), 1, f);
    fclose(f);
    DEBUG_PRINT("[ARCHIVE] Message saved to %s (action=%s)\n", channel_file, msg->action);
}

// Sends Message struct to specified clients
// params:
//      clients - array of all connected Client pointers
//      count - number of clients in array
//      sender - pointer to Client who sent message
//      msg - pointer to Message struct
void broadcast_message(Client **clients, int count, Client *sender, Message *msg, Channel **channels, int num_channels, int exclude_sender)
{
    int sent_count = 0;
    Channel *channel = find_channel_by_name(channels, num_channels, msg->channel);

    for (int i = 0; i < count; i++)
    {

        if (exclude_sender == 1){ // only use for system messages broadcast
            // don't send again to sender 
            if (clients[i]->soc == sender->soc)
            {
                continue;
            }
        }

        // don't send to clients that are not in the channel
        if (IS_BIT_SET(channel->active_members, i) == 0)
        {
            continue;
        }

        // if out_buf has space then copy message into it
        if (clients[i]->out_buf_size + sizeof(Message) < (long unsigned int) CLIENT_OUT_BUF_SIZE)
        {
            memcpy(clients[i]->out_buf + clients[i]->out_buf_size, msg, sizeof(Message));
            clients[i]->out_buf_size += sizeof(Message);
            sent_count++;
        }
        else
        {
            DEBUG_PRINT("[WARN] Client fd=%d out_buf FULL (%d bytes). Message dropped.\n", clients[i]->soc, clients[i]->out_buf_size);
        }
    }
    DEBUG_PRINT("[BROADCAST] Sent to %d clients (excluded sender fd=%d)\n", sent_count, sender->soc);
}

// Sends all of the archived messages to a client
// params: target - the client to receive history
void send_history(Client *target)
{
    char channel_file[strlen(CHANNEL_DIR) + 36];
    strcpy(channel_file, CHANNEL_DIR);
    channel_file[strlen(CHANNEL_DIR)] = '/';
    channel_file[strlen(CHANNEL_DIR) + 1] = '\0';
    strcat(channel_file, target->active_channel->name);
    strcat(channel_file, ".dat");

    FILE *f = fopen(channel_file, "rb");
    if (f == NULL)
    {
        DEBUG_PRINT("[HISTORY] No chat.dat found, skipping history\n");
        return;
    }

    Message hist_msg;
    int count = 0;

    while (fread(&hist_msg, sizeof(Message), 1, f) == 1)
    {
        // mark message as history in case we want to do smthing fancy with chunked-history loading later like discord
        memset(hist_msg.action, 0, 8);
        strcpy(hist_msg.action, "HIST");

        // add it to target's out_buf
        if (target->out_buf_size + sizeof(Message) < (long unsigned int)CLIENT_OUT_BUF_SIZE)
        {
            memcpy(target->out_buf + target->out_buf_size, &hist_msg, sizeof(Message));
            target->out_buf_size += sizeof(Message);
            count++;
        }
        else
        {
            DEBUG_PRINT("[HISTORY] out_buf full, stopping at %d messages\n", count);
            break;
        }
    }

    fclose(f);
    DEBUG_PRINT("[HISTORY] Sent %d messages to client fd=%d\n", count, target->soc);
}

void handle_new_connection(int listen_soc, Client **clients, int *count, int *num_new)
{
    struct sockaddr_in client_addr;
    unsigned int client_len = sizeof(struct sockaddr_in);
    client_addr.sin_family = AF_INET;

    int client_soc = accept(listen_soc, (struct sockaddr *)&client_addr, &client_len);
    if (client_soc == -1)
    {
        perror("accept");
        return;
    }
    DEBUG_PRINT("[CONNECT] Client accepted: fd=%d, total=%d\n", client_soc, *count + 1);

    // Create new client struct
    Client *new_client = malloc(sizeof(Client));
    new_client->soc = client_soc;
    new_client->in_buf_size = 0;
    new_client->out_buf_size = 0;
    new_client->active_channel = NULL;

    clients[*count] = new_client;
    (*num_new)++;

    DEBUG_PRINT("[CONNECT] Sending chat history to new client\n");
    // send_history(new_client);
}

void handle_client_read(Client *client, Client **clients, int *count, int i, Channel **channels, int *num_channels, PrivateChannel **private_channels, int *num_private_channels)
{

    // code to read messages from connections
    int r = read(client->soc, client->in_buf + client->in_buf_size,
                 sizeof(Message) - client->in_buf_size);

    if (r == -1)
    {
        perror("read");
    }
    else if (r == 0)
    {
        DEBUG_PRINT("[DISCONNECT] Client fd=%d | Was client %d of %d\n", client->soc, i, *count);
        printf("Client %d disconnected\n", client->soc);

        // Send disconnect message to all clients in the old channel
        send_disconnect_message(client, clients, *count, channels, *num_channels);

        close(client->soc);
        free(client);

        // remove zombie client from array & shift remaining clients
        for (int k = i; k < *count - 1; k++)
        {
            clients[k] = clients[k + 1];
        }
        clients[*count - 1] = NULL;
        (*count)--;
        i--;
    }
    else
    {

        client->in_buf_size += r;
        if (client->in_buf_size == sizeof(Message))
        {

            // If full message is recieved add it into the output buffers & clean in buffer
            Message *msg = (Message *)client->in_buf;
            // Set message channel
            if (client->active_channel != NULL)
            {
                strncpy(msg->channel, client->active_channel->name, sizeof(msg->channel) - 1);
                msg->channel[sizeof(msg->channel) - 1] = '\0';
            }
            // Set message timestamp
            msg->timestamp = (int64_t)time(NULL);

            DEBUG_PRINT("[RECV] Client fd=%d | Action=%s | DataSize=%d | Data=\"%.50s\"\n", client->soc, msg->action, msg->data_size, msg->data);

            if (strcmp(msg->action, "LOGIN") == 0)
            {
                if (verify_username(client, clients, *count, msg->user) == 1){
                    strcpy(client->user_name, msg->user);
                    DEBUG_PRINT("[LOGIN] Client fd=%d registered as '%s'\n", client->soc, msg->user);
                } else{
                    DEBUG_PRINT("[LOGIN] Client fd=%d failed to register as '%s'\n", client->soc, msg->user);
                }
            }
            else if (strcmp(msg->action, "START") == 0)
            {
                char *channel_name = msg->data;
                if (check_channel_exists(channels, *num_channels, channel_name))
                {
                    respond_to_client_with_message(client, "START:F", 29, "\b\b$ Channel already exists!");
                    DEBUG_PRINT("[START] Client fd=%d failed to start channel '%s'\n", client->soc, channel_name);
                }
                else {
                    respond_to_client_with_message(client, "START:S", 35, "\b\b$ Channel created successfully!");

                    // Send disconnect message to all clients in the old channel
                    send_disconnect_message(client, clients, *count, channels, *num_channels);

                    handle_create_channel(channels, num_channels, channel_name);
                    handle_join_channel(client, channels, *num_channels, channel_name, i);
                    DEBUG_PRINT("[START] Client fd=%d started channel '%s'\n", client->soc, channel_name);
                }
            }
            else if (strcmp(msg->action, "JOIN") == 0)
            {
                char *channel_name = msg->data;
                // Check if allowed to join
                if (check_client_allowed_to_join(client, private_channels, *num_private_channels, channel_name) == 0)
                {
                    respond_to_client_with_message(client, "JOIN:F", 47, "\b\b$ You are not allowed to join this channel!");
                    DEBUG_PRINT("[JOIN] Client fd=%d failed to join channel '%s'\n", client->soc, channel_name);
                }
                else
                {
                    // Handle friends' private channels
                    char *friends_channel_name = check_if_friend_channel(client, private_channels, *num_private_channels, channel_name);
                    if (friends_channel_name != NULL)
                    {
                        strcpy(channel_name, friends_channel_name);
                        free(friends_channel_name);
                    }

                    // Handle normal channels
                    if (check_channel_exists(channels, *num_channels, channel_name))
                    {
                        respond_to_client_with_message(client, "JOIN:S", 34, "\b\b$ Successfully joined channel!");

                        // Send disconnect message to all clients in the old channel
                        send_disconnect_message(client, clients, *count, channels, *num_channels);

                        // Send system message to all clients in the same channel
                        char message[64];      // slightly increased the buffer
                        strcpy(message, "\b\b$ \033[32m"); // display in green colour
                        strcat(message, client->user_name);
                        strcat(message, " joined the channel!\033[0m");
                        handle_join_channel(client, channels, *num_channels, channel_name, i);
                        send_history(client);
                        broadcast_system_message(client, clients, *count, "SYS", message, channels, *num_channels);
                        DEBUG_PRINT("[JOIN] Client fd=%d joined channel '%s'\n", client->soc, channel_name);
                    }
                    else {
                        respond_to_client_with_message(client, "JOIN:F", 24, "\b\b$ Channel not found!");
                        DEBUG_PRINT("[JOIN] Client fd=%d failed to join channel '%s'\n", client->soc, channel_name);
                    }
                }
            }
            else if (strcmp(msg->action, "ONLINE") == 0)
            {
                send_online_users_message(client, clients, *count, channels, *num_channels, private_channels, *num_private_channels);
                DEBUG_PRINT("[ONLINE] Client fd=%d checked who is online\n", client->soc);
            }
            else if (strcmp(msg->action, "FRIEND") == 0)
            {
                handle_friend_request(client, clients, *count, msg->data, channels, num_channels, private_channels, num_private_channels);
                DEBUG_PRINT("[FRIEND] Client fd=%d send a friend request to %s\n", client->soc, msg->data);
            }
            else if (strcmp(msg->action, "LEAVE") == 0)
            {
                // Send disconnect message to all clients in the old channel
                send_disconnect_message(client, clients, *count, channels, *num_channels);

                client->active_channel = NULL;
                respond_to_client_with_message(client, "LEAVE:S", 28, "\b\b$ You left the channel.");
                DEBUG_PRINT("[LEAVE] Client fd=%d left their channel\n", client->soc);
            }
            else
            {
                if (client->active_channel != NULL)
                {
                    archive_message(msg); // saving message to files
                    broadcast_message(clients, *count, client, msg, channels, *num_channels, 0);
                }
            }
            client->in_buf_size = 0;
        }
    }
}

void handle_client_write(Client *client)
{
    // code to boardcast messages
    int w = write(client->soc, client->out_buf, client->out_buf_size);
    if (w == -1)
    {
        perror("write");
    }
    else
    {
        // copy the remainder of the message to the beginning of the buffer
        memmove(client->out_buf, client->out_buf + w, client->out_buf_size - w);
        client->out_buf_size = client->out_buf_size - w;
    }
}

void respond_to_client_with_message(Client *client, char *action, int msg_size, char *msg_data)
{
    Message *message = malloc(sizeof(Message));
    strncpy(message->action, action, sizeof(message->action) * sizeof(char));
    message->data_size = msg_size;
    message->user[0] = '\0';
    if (msg_data != NULL)
    {
        strncpy(message->data, msg_data, sizeof(message->data) * sizeof(char));
        message->data[sizeof(message->data) - 1] = '\0';
    }
    else
    {
        message->data[0] = '\0';
    }
    if (client->out_buf_size + sizeof(Message) < (long unsigned int) CLIENT_OUT_BUF_SIZE)
    {
        memcpy(client->out_buf + client->out_buf_size, message, sizeof(Message));
        client->out_buf_size += sizeof(Message);
    }
    else
    {
        DEBUG_PRINT("[WARN] Client fd=%d out_buf FULL (%d bytes). Message dropped.\n", client->soc, client->out_buf_size);
    }
}

// Broadcasts a system message to all clients in the same channel except the excluded client
void broadcast_system_message(Client *client_to_exclude, Client **clients, int count, char *action, char *msg_data, Channel **channels, int num_channels)
{
    Message *message = malloc(sizeof(Message));
    strncpy(message->action, action, sizeof(message->action) * sizeof(char));
    strncpy(message->channel, client_to_exclude->active_channel->name, sizeof(message->channel) * sizeof(char));
    message->user[0] = '\0'; // system message has no sender
    message->data_size = strlen(msg_data) + 1;
    strncpy(message->data, msg_data, sizeof(message->data) * sizeof(char));
    message->data[strlen(msg_data)] = '\0';

    broadcast_message(clients, count, client_to_exclude, message, channels, num_channels, 1);
}

void send_disconnect_message(Client *client_to_exclude, Client **clients, int count, Channel **channels, int num_channels)
{
    // Send disconnect message to all clients in the old channel
    if (client_to_exclude->active_channel != NULL)
    {
        char message[64];
        // Display in red colour
        strcpy(message, "\b\b$ \033[31m");
        strcat(message, client_to_exclude->user_name);
        strcat(message, " left the channel!\033[0m");
        broadcast_system_message(client_to_exclude, clients, count, "SYS", message, channels, num_channels);
    }
}

void send_online_users_message(Client *client, Client **clients, int count, Channel **channels, int num_channels,  PrivateChannel **private_channels, int num_private_channels) {
    char message_data[2048];  // need larger buffer for ALL channels (just in case)
    int offset = 0;
    
    // looping through ALL channels
    for (int c = 0; c < num_channels; c++) {
        if (check_client_allowed_to_join(client, private_channels, num_private_channels, channels[c]->name) == 0){
            continue; // skip channels client is not allowed to join
        }
        // count users in each channel
        int user_count = 0;
        for (int i = 0; i < count; i++) {
            if (clients[i]->active_channel == channels[c]) {
                user_count++;
            }
        }
        offset += snprintf(message_data + offset, sizeof(message_data) - offset, "%s:%d\n", channels[c]->name, user_count);
    }
    // format it to match what the client expects "channel_name:user_count\n"
    message_data[offset] = '\0';
    respond_to_client_with_message(client, "ONLINE", offset, message_data);
}

void handle_friend_request(Client *client, Client **clients, int count, char *friend_name, Channel **channels, int *num_channels, PrivateChannel **private_channels, int *num_private_channels)
{
    Client *friend = NULL;
    // Find friend
    for (int i = 0; i < count; i++)
    {
        if (strcmp(clients[i]->user_name, friend_name) == 0)
        {
            friend = clients[i];
        }
    }

    if (friend == NULL)
    {
        respond_to_client_with_message(client, "FRIEND", 26, "\b\b$ User does not exist!");
        return;
    }

    // Handle private channel creation
    char new_channel_name[32];
    strcpy(new_channel_name, client->user_name);
    strcat(new_channel_name, friend->user_name);
    Channel *channel = handle_create_channel(channels, num_channels, new_channel_name);
    handle_create_private_channel(client, private_channels, num_private_channels, channel);
    handle_invite_to_private_channel(private_channels, *num_private_channels, new_channel_name, friend->user_name);

    // Notify friend with a system message
    char message[256];
    // Display in pink colour
    strcpy(message, "$ \033[35m");
    strcat(message, client->user_name);
    strcat(message, " sent you a friend request!\n$ Tip: type \"/join ");
    strcat(message, client->user_name);
    strcat(message, "\" to privately message.\033[0m");
    respond_to_client_with_message(friend, "SYS", strlen(message) + 1, message);

    // Send confirmation message to the client
    // Display in pink colour
    strcpy(message, "$ \033[35m");
    strcat(message, "Friend request sent!\n$ Tip: type \"/join ");
    strcat(message, friend->user_name);
    strcat(message, "\" to privately message.\033[0m");
    respond_to_client_with_message(client, "SYS", strlen(message) + 1, message);
}

// Return 1 if username is valid and not taken, otherwise returns 0
int verify_username(Client *client, Client **clients, int count, char* username){
    if (strlen(username) == 0){
         respond_to_client_with_message(client, "LOGN:F", 32, "\b\b$ User name cannot be empty!");
        return 0;
    }
    for (int i = 0; i < count; i++)
    {
        if (strcmp(clients[i]->user_name, username) == 0)
        {
            respond_to_client_with_message(client, "LOGN:F", 30, "\b\b$ User name already taken!");
            return 0;
        }
    }
    respond_to_client_with_message(client, "LOGN:S", 25, "\b\b$ User name is valid!");
    return 1;
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
