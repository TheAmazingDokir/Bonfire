#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h> /* Internet domain header */
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include "entities.h"
#include <termios.h>
#include "constants.h"
#include "constants.c"

#define DEBUG 0  // 1 = debug on, 0 = off

#if DEBUG
#define DEBUG_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...)
#endif

// vars
struct termios term, original;
int in_channel = 0;     // 0 = cant send, 1 = can send
char current_channel[67] = ""; // tracks the current channel

typedef struct {  // storing the channel info
    char name[32];
    int user_count;
} ChannelInfo;

ChannelInfo channel_storage[128];
int channel_storage_count = 0;
int channels_loaded = 0;  // 0 = not loaded, 1 = loaded

// func prototypes
int setup_socket(int *soc, struct sockaddr_in *server);
void setup_select_fds(fd_set *read_fds, int soc, int *max_fd);
int handle_server_message(int soc, Message *soc_msg, char *soc_msg_buf, int *soc_msg_size, char *stdin_msg_buf, int stdin_msg_size);
int handle_user_input(char *c, char *stdin_msg_buf, int *stdin_msg_size);
void build_outgoing_message(Message *stdin_msg, char *stdin_msg_buf, int stdin_msg_size, char *username, int* username_colour);
void send_message_to_server(int soc, char *out_msg_buf);
int command_handler(char *stdin_msg_buf, int stdin_msg_size, int *in_channel, char *current_channel, int soc, int *username_colour);
void request_channels(int soc);
void emoji_parser(char *str);
void render_menu();
void print_ascii_art(char *data);

void handler() { 
    if (tcsetattr(0, TCSANOW, &original) == -1) {
        perror("tcsetattr");
    }
    exit(0);
}

// Utils
void enable_non_canonical_input() {
    if (tcgetattr(0, &term) == -1) {    // get original settings
        perror("tcgetattr");
        exit(1);
    }
    original = term;                     // save original settings
    term.c_lflag &= ~(ICANON | ECHO);    // disable canonical mode and echo
    term.c_cc[VMIN] = 1;                 // require 1 character to read
    term.c_cc[VTIME] = 0;                // no timeout
    tcsetattr(0, TCSANOW, &term);     
    if (tcsetattr(0, TCSANOW, &term) == -1) { // apply new settings
        perror("tcsetattr");
        exit(1);
    }
}

void disable_non_canonical_input() {
    if (tcsetattr(0, TCSANOW, &original) == -1) {
        perror("tcsetattr");
        exit(1);
    }
}

void get_username(char *username, int max_len) {
    printf("Enter Username: ");
    fflush(stdout);

    char input[256];
    if (fgets(input, sizeof(input), stdin) != NULL) {

        // delete newline
        input[strcspn(input, "\n")] = 0;

        // check if empty or just whitespace
        int is_empty = 1;
        for (int i = 0; i < strlen(input); i++) {
            if (input[i] != ' ' && input[i] != '\t') {
                is_empty = 0;
                break;
            }
        }

        if (is_empty || strlen(input) == 0) {
            strcpy(username, "Anon");
        } else {
            strncpy(username, input, max_len - 1);
            username[max_len - 1] = '\0';
        }
    } else {
        strcpy(username, "Anon");
    }
}

// Returns -1 if color is not found
int get_code_from_colour_name(char *colour, int colour_len)
{
    for (int i = 0; i < NUMBER_OF_TEXT_COLOURS; i++)
    {
        if (strncmp(TEXT_COLOURS[i].name, colour, colour_len - 1) == 0)
        {
            return TEXT_COLOURS[i].code;
        }
    }
    return -1;
}

int get_random_username_colour()
{
    srand(time(NULL));
    return TEXT_COLOURS[rand() % NUMBER_OF_TEXT_COLOURS].code;
}

int main() {
    // create socket
    int soc;
    struct sockaddr_in server;
    if (setup_socket(&soc, &server) == -1) {
        exit(1);
    }

    // user login (has to be before canonical input otherwise gets messed up)
    char username[16];
    int username_colour;
    get_username(username, 16);
    username_colour = get_random_username_colour();
    DEBUG_PRINT("[CLIENT] Username: '%s'\n", username);
    enable_non_canonical_input();
    request_channels(soc);

    // send login to server
    Message *login_msg = malloc(sizeof(Message));
    if (login_msg == NULL) {
        perror("malloc");
        exit(1);
    }

    memset(login_msg, 0, sizeof(Message));
    strcpy(login_msg->action, "LOGIN");
    strcpy(login_msg->user, username);
    login_msg->data_size = strlen(username);
    write(soc, login_msg, sizeof(Message));
    free(login_msg);

    fd_set read_fds;
    int max_fd;

    Message *soc_msg = malloc(sizeof(Message));
    if (soc_msg == NULL) { 
        perror("malloc"); 
        exit(1); 
    }

    char soc_msg_buf[sizeof(Message)];
    int soc_msg_size = 0;

    Message *stdin_msg = malloc(sizeof(Message));
    if (stdin_msg == NULL) { 
        perror("malloc"); 
        exit(1); 
    }

    char stdin_msg_buf[256];
    int stdin_msg_size = 0;

    char out_msg_buf[sizeof(Message)];

    // fetch all the channel info
    for (int attempts = 0; attempts < 20; attempts++) {
        fd_set test_fds;
        FD_ZERO(&test_fds);
        FD_SET(soc, &test_fds);
        struct timeval tv = {0, 10000};  // 10ms timeout to grab the info
        if (select(soc + 1, &test_fds, NULL, NULL, &tv) > 0) {
            
            // process refresh info if needed.
            handle_server_message(soc, soc_msg, soc_msg_buf, &soc_msg_size, stdin_msg_buf, 0);
        }
    usleep(10000);  // 10ms delay between each check
    }

    //usleep(10000); <-- doesnt work lol
    render_menu();
    printf("> ");
    fflush(stdout);

    while (1) {
        setup_select_fds(&read_fds, soc, &max_fd);

        // select between the socket input and std input
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) == -1) {
            perror("select");
            exit(1);
        }

        // do message reading and printing from socket
        if (FD_ISSET(soc, &read_fds)) {
            handle_server_message(soc, soc_msg, soc_msg_buf, &soc_msg_size, stdin_msg_buf, stdin_msg_size);
        }

        // do message reading and sending from input
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char c;
            if (handle_user_input(&c, stdin_msg_buf, &stdin_msg_size)) {
                if (stdin_msg_size <= 1 && c == '\n') {  // empty message moment
                    printf("> ");
                    fflush(stdout);
                    stdin_msg_size = 0;
                    continue;
                }

                // handle command s& check if msg should be sent
                int shouldSend = command_handler(stdin_msg_buf, stdin_msg_size, &in_channel, current_channel, soc, &username_colour);
                if (shouldSend == 1) {
                    build_outgoing_message(stdin_msg, stdin_msg_buf, stdin_msg_size, username, &username_colour);
                    
                    // update local channel tracking
                    if (strncmp(stdin_msg_buf, "/join", 5) == 0) {
                        in_channel = 1;
                        strncpy(current_channel, stdin_msg_buf + 6, 31);
                        current_channel[31] = '\0';
                        request_channels(soc);
                        render_menu();
                        //printf("Joined channel: %s", current_channel);    
                    } else if (strncmp(stdin_msg_buf, "/create", 7) == 0) {
                        in_channel = 1;
                        strncpy(current_channel, stdin_msg_buf + 8, 31);
                        current_channel[31] = '\0';
                        request_channels(soc);
                        render_menu();
                        //printf("Created channel: %s", current_channel);               <----- sure ill let server handle these.
                    }
                    
                    memcpy(out_msg_buf, stdin_msg, sizeof(Message)); // send message to the socket
                    send_message_to_server(soc, out_msg_buf);

                    // ANSI escape to move up one line, clear it (this is to make emoji work for sender)
                    if (strcmp(stdin_msg->action, "SEND") == 0) {
                        printf("\033[A\033[2K");  
                        //printf("\n");
                    }
                }

                // clean up the message and buffer
                stdin_msg_size = 0;
                memset(stdin_msg_buf, 0, 256);
                printf("> ");
                fflush(stdout);
            }
        }
    }

    free(soc_msg);
    free(stdin_msg);
    disable_non_canonical_input();
    return 0;
}


// renders the main menu w/ channel list & misc
void render_menu() {
    // clear screen with a bunch of newlines
    for (int i = 0; i < 5; i++) printf("\n");
    
    printf("⠀⠀⠀⠀⠀⠀⠀⠀⢱⣄⠀⠀⠀⠀⠀⠀⠀⢱⣄⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀\n");
    printf("⠀⠀⠀⠀⠀⠀⠀⠀⢸⣿⡄⠀⠀⡀⠀⠀⠀⢸⣿⣷⡄⠀⠀⠀⠀⠀⠀⠀⠀⠀\n");
    printf("⠀⠀⠀⠀⠀⠀⠀⠀⠈⢿⠇⠀⠀⣷⣦⣤⣴⣿⣿⣿⣷⠀⠀⠀⠀⠀⠀⠀⠀⠀\n");
    printf("⠀⠀⠀⠀⠀⠀⠀⠀⡀⠀⠀⠀⠀⣿⣿⣿⣿⣿⣿⣿⡿⠀⠀⠀⠀⠀⠀⠀⠀⠀\n");
    printf("⠀⠀⠀⠀⠀⠀⢠⣾⣷⡀⠀⠀⣸⣿⣿⣿⣿⣿⣿⣿⠃⠀⠀⠀⠀⠀⠀⠀⠀⠀\n");
    printf("⠀⠀⠀⠀⠀⢰⣿⣿⣿⣿⣷⣿⣿⣿⡟⢿⠿⠋⣿⣿⠀⠀⠀⠀⠰⣄⠀⠀⠀⠀\n");
    printf("⠀⠀⠀⠀⠀⣾⣿⣿⣿⣿⣿⣿⣿⣿⠃⠀⠀⠀⣿⣿⣦⡀⠀⠀⢀⣿⣧⠀⠀⠀\n");
    printf("⠀⠀⠀⠀⠀⢹⣿⣿⣿⠙⠻⠟⠋⠁⠀⠀⠀⠀⢿⣿⣿⣿⣶⣶⣿⣿⣿⠀⠀⠀\n");
    printf("⠀⠀⠀⠀⠀⠈⣿⣿⣿⡆⠀⠀⢀⣠⡤⠀⠀⠀⠈⠻⣿⡿⣿⣿⣿⣿⡟⠀⠀⠀\n");
    printf("⠀⠀⠀⠀⠀⠀⠘⠟⠋⣁⣤⡾⢟⣩⣴⣶⣆⠀⠀⠀⠀⢠⣿⣿⣿⠟⠀⠀⠀⠀\n");
    printf("⠀⠀⠀⠀⠀⢀⣠⣴⣿⣿⣿⣿⠟⢉⣁⣀⣉⠀⢹⣶⣶⣤⣄⣈⡁⠀⠀⠀⠀⠀\n");
    printf("⠀⠀⠀⠀⢸⣿⣿⣿⣿⢿⣿⠇⢰⡟⣫⣦⠙⢷⡀⣿⣿⣯⣍⣛⠛⠋⠀⠀⠀⠀\n");
    printf("⠀⠀⠀⠀⠀⠛⢋⣡⡾⠟⠋⠀⠸⣧⣙⡟⢁⡾⠁⠿⢿⣿⣿⣭⡉⠁⠀⠀⠀⠀\n");
    printf("⠀⠀⠀⠀⠀⠀⠈⠁⠀⠀⠀⠀⠀⠈⠙⠛⠋⠀⠀⠀⠀⠀⠈⠉⠉⠀⠀⠀⠀⠀\n");
    printf("=====================================\n");
    printf("Welcome to bonfire.\n");
    printf("Here's some channels to check out:\n");

    // note: storage is refreshed pre-call to this func to keep it param-less
    if (channel_storage_count == 0) {
        printf("No channels available.\n");
    } else {
        for (int i = 0; i < channel_storage_count && i < 5; i++) {
            printf("%s [%d online]\n", channel_storage[i].name, channel_storage[i].user_count);
        }
        if (channel_storage_count > 5) {
            printf("...and %d more\n", channel_storage_count - 5);
        }
    }

    printf("=====================================\n");
    printf("Tip: Send /help for list of commands.\n");
    fflush(stdout);
}

// ssets up the fd_set for select() call
// params: 
//      soc - pointer to socket fd
//      server - server address struct
// return: 0 on success, -1 on error
int setup_socket(int *soc, struct sockaddr_in *server) {

    // create socket
    *soc = socket(AF_INET, SOCK_STREAM, 0);
    if (*soc == -1) {
        perror("client: socket");
        return -1;
    }
    DEBUG_PRINT("[SOCKET] Created: fd=%d\n", *soc);

    // initialize server address
    server->sin_family = AF_INET;
    server->sin_port = htons(51984);
    memset(&server->sin_zero, 0, 8);
    struct addrinfo *ai;

    // get hostname of current device
    char hostname[1024];
    if (gethostname(hostname, sizeof(hostname)) == -1) {
        perror("gethostname");
    return -1;
}

    // this call declares memory and populates ailist
    if (getaddrinfo(hostname, NULL, NULL, &ai) != 0) {
        perror("getaddrinfo");
        return -1;
    }
    server->sin_addr = ((struct sockaddr_in *)ai->ai_addr)->sin_addr;

    // free the memory that was allocated by getaddrinfo for this list
    freeaddrinfo(ai);

    int ret = connect(*soc, (struct sockaddr *)server, sizeof(struct sockaddr_in));
    if (ret == -1) {
        DEBUG_PRINT("[CONNECT] Server offline, waiting...\n");
        printf("$ Server is currently offline.\n");
        printf("$ Awaiting connection... \n");

        while (ret == -1) {
            ret = connect(*soc, (struct sockaddr *)server, sizeof(struct sockaddr_in));
            sleep(1);
        }
    }

    DEBUG_PRINT("[CONNECT] Connected to server!\n");
    printf("$ Connected to the server! \n");
    return 0;
}


// makes socket and connects to server
// params: 
//      read_fds - fd_set pointer
//      soc - pointer to socket fd
//      max_fd - pointer to max fd value
void setup_select_fds(fd_set *read_fds, int soc, int *max_fd) {
    // Clean fds sets
    FD_ZERO(read_fds);

    // set socket to be read fd
    FD_SET(soc, read_fds);
    FD_SET(STDIN_FILENO, read_fds);

    if (soc > STDIN_FILENO) {
        *max_fd = soc;
    } else {
        *max_fd = STDIN_FILENO;
    }
}

// reads and prints the incoming mgs from srever
// params: 
//      soc - socket fd
//      soc_msg - msg struct
//      soc_msg_buf - read buffer
//      soc_msg_size - pointer to buffer size counter
//      stdin_msg_buf - current input buffer
//      stdin_msg_size - current input length
// return: 1 if processed, 0 if buffering
int handle_server_message(int soc, Message *soc_msg, char *soc_msg_buf, int *soc_msg_size, char *stdin_msg_buf, int stdin_msg_size) {
    DEBUG_PRINT("[HANDLE] soc_msg_size=%d, sizeof(Message)=%zu\n", *soc_msg_size, sizeof(Message));
    if (*soc_msg_size < sizeof(Message)) {
        int r = read(soc, soc_msg_buf + *soc_msg_size, sizeof(Message) - *soc_msg_size);
        
        if (r == -1) {
            perror("read");
            return 0;
        }

        if (r == 0) {
            DEBUG_PRINT("[RECV] Server disconnected\n"); // server closed connection
            return 0;
        }
        
        *soc_msg_size += r;
    }

    if (*soc_msg_size == sizeof(Message)) {
        memcpy(soc_msg, soc_msg_buf, sizeof(Message));

        if (soc_msg->data_size > 0) {
            soc_msg->data[soc_msg->data_size - 1] = '\0';
        }

        DEBUG_PRINT("[RECV] Action=%s | DataSize=%d | Data=\"%.50s\"\n", soc_msg->action, soc_msg->data_size, soc_msg->data);

        // ignore chat messages when user in main menu
        if (in_channel == 0 && strcmp(soc_msg->action, "SEND") == 0) {
            DEBUG_PRINT("[RECV] IGNORED (not in channel)\n");
            *soc_msg_size = 0;
            return 0;  // throw it away lol
        }

        // erase the current input
        for (int i = 0; i < stdin_msg_size; i++) {
            printf("\b");
        }

        emoji_parser(soc_msg->data);

        // parse the ONLINE msg from the server
        if (strcmp(soc_msg->action, "ONLINE") == 0) {   
            DEBUG_PRINT("[ONLINE] Parsing: %s\n", soc_msg->data);
            
            channel_storage_count = 0;
            char *line = strtok(soc_msg->data, "\n");
            while (line != NULL && channel_storage_count < 128) {
                char *colon = strchr(line, ':');
                if (colon != NULL) {
                    *colon = '\0';
                    strncpy(channel_storage[channel_storage_count].name, line, 31);
                    channel_storage[channel_storage_count].name[31] = '\0';
                    channel_storage[channel_storage_count].user_count = atoi(colon + 1);
                    channel_storage_count++;
                }
                line = strtok(NULL, "\n");
            }
            channels_loaded = 1;

            // reprint the > and the current input thingie
            // printf("> ");
            for (int i = 0; i < stdin_msg_size; i++) {
                printf("%c", stdin_msg_buf[i]);
            }
            fflush(stdout);
            *soc_msg_size = 0;      // clean the buffer!!!!!!!

            DEBUG_PRINT("[ONLINE] Done parsing, cleaned buffer, returning\n");
            return 1;  // do not print it as a message lol
        }


        // add username to msg
        if (strlen(soc_msg->user) > 0) {
            if (soc_msg->username_colour > 0) {
                printf(COLOURED_USER_NAME, soc_msg->username_colour, soc_msg->user, soc_msg->data);
            }
            else {
                printf("[%s] %s\n", soc_msg->user, soc_msg->data);
            }

            // Convert timestamp to local
            struct tm *time = localtime((time_t*)&soc_msg->timestamp);
            // Convet to time string
            char time_str[16];
            strftime(time_str, sizeof(time_str), "%I:%M %p", time);
            // For user messages also print time
            printf("\033[90m%8s\033[0m\n", time_str);
        } else {
            printf("%s\n", soc_msg->data);
        }

        print_ascii_art(soc_msg->data);

        printf("> ");
        // reprint the current input after printing the message
        for (int i = 0; i < stdin_msg_size; i++) {
            printf("%c", stdin_msg_buf[i]);
        }
        fflush(stdout);

        // Clean up the message and the buffer
        *soc_msg_size = 0;
        return 1;
    }
    DEBUG_PRINT("[HANDLE] Returning, soc_msg_size now=%d\n", *soc_msg_size);
    return 0;
}


// reads user input and handles backspaces
// params:
//      c - pointer to char read
//      stdin_msg_buf - input buffer
//      stdin_msg_size - pointer to buffer size counter
// return: 1 if hit newline, 0 if not yet
int handle_user_input(char *c, char *stdin_msg_buf, int *stdin_msg_size) {
    *c = getchar();
    if (*c == 127 || *c == '\b') {
        // erase on backspace
        if (*stdin_msg_size > 0) {
            printf("\b \b");
            fflush(stdout);
            (*stdin_msg_size)--;
        }
    } else {
        stdin_msg_buf[*stdin_msg_size] = *c;
        *stdin_msg_size += 1;
        printf("%c", *c);
        fflush(stdout);
    }

    if (*c == '\n' || *stdin_msg_size == 256) {

        return 1;  // complete message
    }
    return 0;  // not yet
}


// parses user input and builds the Message struct (for generic messages and commands that talk to server)
// params: 
//          stdin_msg - output message struct
//          stdin_msg_buf - input buffer
//          stdin_msg_size - input length
//          username - client username
//          username_colour - client color pointer
void build_outgoing_message(Message *stdin_msg, char *stdin_msg_buf, int stdin_msg_size, char *username, int* username_colour) {
    if (stdin_msg_size <= 1 && stdin_msg_buf[0] == '\n') {
        return; // empty message moment
    }

    if (strncmp(stdin_msg_buf, "/join", 5) == 0) {
        memset(stdin_msg, 0, sizeof(Message)); // remove garbage
        strcpy(stdin_msg->action, "JOIN");
        strcpy(stdin_msg->user, username);
        strncpy(stdin_msg->data, stdin_msg_buf + 6, stdin_msg_size - 7);
        stdin_msg->data[stdin_msg_size - 7] = '\0';
        stdin_msg->data_size = stdin_msg_size - 7;
    } else if (strncmp(stdin_msg_buf, "/create", 7) == 0) {
        memset(stdin_msg, 0, sizeof(Message)); // remove garbage
        strcpy(stdin_msg->action, "START");
        strcpy(stdin_msg->user, username);
        strncpy(stdin_msg->data, stdin_msg_buf + 8, stdin_msg_size - 9);
        stdin_msg->data[stdin_msg_size - 9] = '\0';
        stdin_msg->data_size = stdin_msg_size - 9;
    } else if (strncmp(stdin_msg_buf, "/friend", 7) == 0) {
        memset(stdin_msg, 0, sizeof(Message)); // remove garbage
        strcpy(stdin_msg->action, "FRIEND");
        strcpy(stdin_msg->user, username);
        strncpy(stdin_msg->data, stdin_msg_buf + 8, stdin_msg_size - 8);
        stdin_msg->data[stdin_msg_size - 9] = '\0';
        stdin_msg->data_size = stdin_msg_size - 8;
    } else {
        memset(stdin_msg, 0, sizeof(Message)); // remove garbage
        strcpy(stdin_msg->action, "SEND");
        strcpy(stdin_msg->user, username);
        stdin_msg->username_colour = *username_colour;
        strncpy(stdin_msg->data, stdin_msg_buf, stdin_msg_size);
        stdin_msg->data[stdin_msg_size - 1] = '\0';
        stdin_msg->data_size = stdin_msg_size;
    }
    DEBUG_PRINT("[SEND] Action=%s | DataSize=%d | Colour=%d | Data=\"%.50s\"\n", stdin_msg->action, stdin_msg->data_size, stdin_msg->username_colour, stdin_msg->data);
}



// sends the Message struct to server socket
// params: 
//      soc - socket fd
//      out_msg_buf - buffer with message
void send_message_to_server(int soc, char *out_msg_buf) {
    int count = 0;
    while (count < sizeof(Message)) {
        int w = write(soc, out_msg_buf + count, sizeof(Message) - count);
        if (w == -1) {
            perror("write");
            return; 
        }
        count += w;
    }
}


// handles the client side commands (/join, /create, /leave, /channels, etc)
// params: 
//      stdin_msg_buf - input buffer
//      stdin_msg_size - input length
//      in_channel - pointer to channel state
//      current_channel - pointer to current channel name
// return: 1 if message should be sent to server, 0 if handled client side
int command_handler(char *stdin_msg_buf, int stdin_msg_size, int *in_channel, char *current_channel, int soc, int *username_colour) {    
    
    // leave command 
    if (strncmp(stdin_msg_buf, "/leave", 6) == 0) {
        DEBUG_PRINT("[CMD] /leave triggered\n");

        if (*in_channel == 1 && strlen(current_channel) > 0) {
            Message *leave_msg = malloc(sizeof(Message));
            if (leave_msg == NULL) {
                perror("malloc");
                return 0;
            }
            memset(leave_msg, 0, sizeof(Message));
            strcpy(leave_msg->action, "LEAVE");
            strncpy(leave_msg->data, current_channel, 31);
            leave_msg->data[31] = '\0';
            leave_msg->data_size = strlen(current_channel);
            
            char leave_buf[sizeof(Message)];
            memcpy(leave_buf, leave_msg, sizeof(Message));
            send_message_to_server(soc, leave_buf);
            free(leave_msg);
            
            DEBUG_PRINT("[LEAVE] Sent LEAVE for channel: %s\n", current_channel);
        }       

        *in_channel = 0;
        current_channel[0] = '\0';
        request_channels(soc);
        render_menu();
        return 0;  // dont send to server, its implicitly sent
    }

    // help command
    if (strncmp(stdin_msg_buf, "/help", 5) == 0) {
        DEBUG_PRINT("[CMD] /help triggered\n");
        printf("$ Available commands:\n");
        printf("$   /join    <channel>  - Join a channel\n");
        printf("$   /leave              - Leave current channel\n");
        printf("$   /create  <channel>  - Create a channel\n");
        printf("$   /channels           - List active channels\n");
        printf("$   /friend  <user>     - Friend user for direct messages\n");
        printf("$   /color   <color>    - Change your username color\n");
        printf("$   /colorlist          - Show available colors\n");
        printf("$   /emoji              - Show available emoji\n");
        printf("$   /ascii              - Show ascii art commands\n");
        printf("$   /exit               - Exit the program\n");
        return 0; 
    }

    // emoji command
    if (strncmp(stdin_msg_buf, "/emoji", 6) == 0) {
        DEBUG_PRINT("[CMD] /emoji triggered\n");
        printf("$ Available emoji:\n");
        printf("$   :fire:   🔥   :flush: 😳   :joy:    😂\n");
        printf("$   :skull:  💀   :vomit: 🤮   :flower: 🥀\n");
        printf("$   :camera: 📷   :gun:   🔫   :eyes:   👀\n");
        printf("$   :cheese: 🧀   :sob:   😭   :moai:   🗿\n");
        printf("$ Tip: Type emoji codes in messages, e.g., 'hello :fire: world'\n");
        return 0;
    }

    // ascii command
    if (strncmp(stdin_msg_buf, "/ascii", 6) == 0) {
        DEBUG_PRINT("[CMD] /ascii triggered\n");
        printf("$ Available ASCII Art Stickers:\n");
        printf("$   /troll      - Troll face.\n");
        printf("$   /chad       - Gigachad.\n");
        printf("$   /moai       - Moai.\n");
        printf("$   /yes        - Yes.\n");
        printf("$   /pony       - Friendship is magic!\n");
        printf("$   /apple      - Why is it bad?\n");
        printf("$   /amongus    - Are you sus?\n");
        printf("$   /squidward  - Handsome.\n");
        printf("$ Tip: Type the command to send the ASCII art to chat\n");
        return 0;
    }

    // colorlist command
    if (strncmp(stdin_msg_buf, "/colorlist", 10) == 0) {
        DEBUG_PRINT("[CMD] /colorlist triggered\n");
        printf("$ Available username colors:\n");
        printf("$   \033[31mred\033[0m     \033[32mgreen\033[0m    \033[33myellow\033[0m\n");
        printf("$   \033[34mpurple\033[0m  \033[35mpink\033[0m     \033[36mblue\033[0m\n");
        printf("$   \033[37mwhite\033[0m\n");
        printf("$ Tip: Use /color <name> to change your color\n");
        printf("> ");
        return 0;
    }

    // change color command
    if (strncmp(stdin_msg_buf, "/color", 6) == 0) {
        int code = get_code_from_colour_name(stdin_msg_buf + 7, stdin_msg_size - 7);
        if (code == -1) {
            printf("Invalid color!\n");
        }
        else {
            *username_colour = code;
            printf("Colour changed successfully!\n");
        }
        DEBUG_PRINT("[/COLOR] Colour=%d\n", *username_colour);
        return 0; // dont send to server
    }

    // exit command
    if (strncmp(stdin_msg_buf, "/exit", 5) == 0) {
        DEBUG_PRINT("[CMD] /exit triggered - shutting down\n");
        printf("$ Goodbye!\n");
        disable_non_canonical_input();  // restore terminal input
        exit(0);  // terminate the client
    }

    // channels command
    if (strncmp(stdin_msg_buf, "/channels", 9) == 0) {
        DEBUG_PRINT("[CMD] /channels triggered\n");

        //refresh the info
        request_channels(soc);
        // usleep(100000); - doesnt work, just refresh a bunch prior to menu print.

        // print out all the channels (maybe we should limit it to top 5?)
        printf("$ Available channels:\n");
        if (channel_storage_count == 0) {
            printf("$ No channels available. Create your own with /create\n");
        } else {
            for (int k = 0; k < channel_storage_count; k++) {
                printf("$   %s [%d online]\n", channel_storage[k].name, channel_storage[k].user_count);
            }
        }
        // printf("> ");
        // fflush(stdout);
        return 0;
    }   

    // let /join and /create even when not in channel
    if (strncmp(stdin_msg_buf, "/join", 5) == 0 || strncmp(stdin_msg_buf, "/create", 7) == 0 || strncmp(stdin_msg_buf, "/channels", 9) == 0) {
        DEBUG_PRINT("[CMD] /join or /create or /channels allowed\n");
        return 1;  // allow send to server
    }

    // regular message in main menu
    if (*in_channel == 0) {
        DEBUG_PRINT("[CMD] Message blocked\n");
        printf("$ Join a channel first! Use /join <channel>\n");
        return 0;  // block the send
    }

    return 1;  // allow the send
}


// requests the list of line channels + user counts for each
// params: soc - socket fd
void request_channels(int soc) {
    Message *msg = malloc(sizeof(Message));
    if (msg == NULL) {
        perror("malloc");
        return;
    }
    char out_buf[sizeof(Message)];
    
    memset(msg, 0, sizeof(Message));
    strcpy(msg->action, "ONLINE");
    msg->data_size = 0;
    
    memcpy(out_buf, msg, sizeof(Message));
    
    int count = 0;
    while (count < sizeof(Message)) {
        int w = write(soc, out_buf + count, sizeof(Message) - count);
        if (w == -1) {
            perror("write");
            free(msg);
            return;
        }
        count += w;
    }
    
    free(msg);
}





// replaces emoji codes with unicode characters client side
// params: str - string to parse
void emoji_parser(char *str) {
    // table for emoji info: code, utf-8 bytes, length
    struct { 
        char *code; 
        char utf8[5]; 
        int len; 
    } emoji[] = {
        {":fire:",      "\xF0\x9F\x94\xA5", 4}, // 🔥
        {":flush:",     "\xF0\x9F\x98\xB3", 4}, // 😳
        {":joy:",       "\xF0\x9F\x98\x82", 4}, // 😂
        {":skull:",     "\xF0\x9F\x92\x80", 4}, // 💀
        {":vomit:",     "\xF0\x9F\xA4\xAE", 4}, // 🤮
        {":flower:",    "\xF0\x9F\xA5\x80", 4}, // 🥀
        {":camera:",    "\xF0\x9F\x93\xB7", 4}, // 📷
        {":gun:",       "\xF0\x9F\x94\xAB", 4}, // 🔫
        {":eyes:",      "\xF0\x9F\x91\x80", 4}, // 👀
        {":cheese:",    "\xF0\x9F\xA7\x80", 4}, // 🧀
        {":sob:",       "\xF0\x9F\x98\xAD", 4}, // 😭
        {":moai:",      "\xF0\x9F\x97\xBF", 4}, // 🗿
        {NULL,          "", 0}
    };
    
    // process each emoji types
    for (int e = 0; emoji[e].code != NULL; e++) {
        char *pos = strstr(str, emoji[e].code);
        while (pos != NULL) {
            int code_len = strlen(emoji[e].code);
            char *rest = pos + code_len;
            memmove(pos + emoji[e].len, rest, strlen(rest) + 1);
            memcpy(pos, emoji[e].utf8, emoji[e].len);
            DEBUG_PRINT("[EMOJI] Replaced %s with unicode\n", emoji[e].code);
            pos = strstr(str, emoji[e].code);
        }
    }
}

void print_ascii_art(char *data) {
    if (strcmp(data, "/squidward") == 0) {
        printf("⠀⠀⠀⠀⠀⠀⣠⠴⠒⠉⠉⠉⠉⠉⠓⠢⣄⠀⠀⠀\n");
        printf("⠀⠀⠀⠀⠀⡞⠁⠀⢀⡀⠀⠀⠀⠀⡄⠀⠈⣆⠀⠀\n");
        printf("⠀⠀⠀⠀⢸⠁⠀⠀⠀⣱⠀⠀⠀⠐⣣⠂⠀⢹⡀⠀\n");
        printf("⠀⠀⠀⠀⢸⠀⠀⠀⠀⢇⢠⣤⣴⣕⢹⢶⣄⣠⠇⠀\n");
        printf("⠀⠀⠀⠀⠈⢧⠀⠀⡰⠀⠋⢣⣽⠎⠈⣿⡟⢻⠀⠀\n");
        printf("⠀⠀⠀⠀⠀⠀⠓⠦⡷⣄⣀⠀⣰⡀⠀⡇⢀⠞⠀⠀\n");
        printf("⠀⠀⠀⠀⠀⠀⠀⡸⠁⡇⠸⢰⠉⡩⢶⡹⡇⠀⠀⠀\n");
        printf("⠀⣀⠤⠖⠒⡿⠹⠁⢰⠁⠀⠈⠈⢯⣭⠃⡇⠀⠀⠀\n");
        printf("⣋⠀⠀⠀⢰⠁⠀⠀⠘⠦⣄⡀⠀⠈⠐⡆⢙⣆⠀⠀\n");
        printf("⠉⠉⠳⣄⠀⢇⢠⡄⠀⠀⠀⠉⣳⠒⠒⡻⠉⠘⠓⢆\n");
    } else if (strcmp(data, "/chad") == 0) {
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⡿⠿⠛⠛⠛⠋⠉⠈⠉⠉⠉⠉⠛⠻⢿⣿⣿⣿⣿⣿⣿⣿ \n");
        printf("⣿⣿⣿⣿⣿⡿⠋⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠉⠛⢿⣿⣿⣿⣿⠀\n");
        printf("⣿⣿⣿⣿⡏⣀⠀⠀⠀⠀⠀⠀⠀⣀⣤⣤⣤⣄⡀⠀⠀⠀⠀⠀⠀⠀⠙⢿⣿⣿⠀\n");
        printf("⣿⣿⣿⢏⣴⣿⣷⠀⠀⠀⠀⠀⢾⣿⣿⣿⣿⣿⣿⡆⠀⠀⠀⠀⠀⠀⠀⠈⣿⣿⠀\n");
        printf("⣿⣿⣟⣾⣿⡟⠁⠀⠀⠀⠀⠀⢀⣾⣿⣿⣿⣿⣿⣷⢢⠀⠀⠀⠀⠀⠀⠀⢸⣿⠀\n");
        printf("⣿⣿⣿⣿⣟⠀⡴⠄⠀⠀⠀⠀⠀⠀⠙⠻⣿⣿⣿⣿⣷⣄⠀⠀⠀⠀⠀⠀⠀⣿⠀\n");
        printf("⣿⣿⣿⠟⠻⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠶⢴⣿⣿⣿⣿⣿⣧⠀⠀⠀⠀⠀⠀⣿⠀\n");
        printf("⣿⣁⡀⠀⠀⢰⢠⣦⠀⠀⠀⠀⠀⠀⠀⠀⢀⣼⣿⣿⣿⣿⣿⡄⠀⣴⣶⣿⡄⣿⠀\n");
        printf("⣿⡋⠀⠀⠀⠎⢸⣿⡆⠀⠀⠀⠀⠀⠀⣴⣿⣿⣿⣿⣿⣿⣿⠗⢘⣿⣟⠛⠿⣼⠀\n");
        printf("⣿⣿⠋⢀⡌⢰⣿⡿⢿⡀⠀⠀⠀⠀⠀⠙⠿⣿⣿⣿⣿⣿⡇⠀⢸⣿⣿⣧⢀⣼⠀\n");
        printf("⣿⣿⣷⢻⠄⠘⠛⠋⠛⠃⠀⠀⠀⠀⠀⢿⣧⠈⠉⠙⠛⠋⠀⠀⠀⣿⣿⣿⣿⣿⠀\n");
        printf("⣿⣿⣧⠀⠈⢸⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠟⠀⠀⠀⠀⢀⢃⠀⠀⢸⣿⣿⣿⣿⠀\n");
        printf("⣿⣿⡿⠀⠴⢗⣠⣤⣴⡶⠶⠖⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣀⡸⠀⣿⣿⣿⣿⠀\n");
        printf("⣿⣿⣿⡀⢠⣾⣿⠏⠀⠠⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠛⠉⠀⣿⣿⣿⣿⠀\n");
        printf("⣿⣿⣿⣧⠈⢹⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣰⣿⣿⣿⣿⠀\n");
        printf("⣿⣿⣿⣿⡄⠈⠃⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣠⣴⣾⣿⣿⣿⣿⣿⠀\n");
        printf("⣿⣿⣿⣿⣧⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣠⣾⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀\n");
        printf("⣿⣿⣿⣿⣷⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣴⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀\n");
        printf("⣿⣿⣿⣿⣿⣦⣄⣀⣀⣀⣀⠀⠀⠀⠀⠘⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣷⡄⠀⠀⠀⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀\n");
    } else if (strcmp(data, "/troll") == 0) {
        printf("⠀⠀⠀⠀⠀⠀⠀⣠⣤⣤⣤⡤⢤⣤⣤⣤⣤⣤⣄⣀⣀⡀⠀⠀⠀⠀⠀⠀⠀⠀\n");
        printf("⠀⠀⠀⠀⠀⣠⣿⡿⣟⠯⡒⢯⣽⣓⣒⢾⣯⣭⣿⣿⠿⠭⠭⣯⣷⣦⡀⠀⠀⠀\n");
        printf("⠀⠀⠀⠀⣰⣿⣯⣞⣕⣽⠾⠿⠿⠿⢿⣏⣿⣿⣿⡗⣽⣿⣿⣷⡝⣿⣿⡆⠀⠀\n");
        printf("⠀⠀⠀⣀⣛⠛⢿⣛⢝⢁⣀⣀⣀⠓⠶⠈⣿⣿⡿⠗⠉⠁⢀⣀⣹⣛⣛⣳⢄⠀\n");
        printf("⠀⡔⡾⢁⣴⡆⢦⣬⣙⣛⣋⣤⣿⣿⣷⣾⣿⣿⣿⡆⢿⣿⡟⠻⠛⡉⣍⣲⢱⠁\n");
        printf("⠀⣇⣇⢸⣉⡀⢦⣌⡙⠻⠿⣯⣭⣥⠡⡤⠿⢿⣿⣿⡆⠉⡻⢿⣿⠇⢻⣟⠼⠀\n");
        printf("⠀⠈⠪⣴⣿⣧⡀⢉⠛⠘⢶⣦⣬⠉⣀⠓⠿⠿⠯⢉⣴⠿⠿⠓⡁⡄⠀⣿⠃⠀\n");
        printf("⠀⠀⠀⠙⣿⣿⣷⣌⠻⢠⣤⣀⠉⠐⠛⠿⠿⠰⠶⠦⠰⠶⠇⠘⠃⠁⠀⣿⠀⠀\n");
        printf("⠀⠀⠀⠀⠘⢿⣿⣿⣷⣌⠻⢿⠇⣼⣶⣦⡄⣄⣀⡀⢀⡀⢀⡀⡀⠀⢠⣿⠀⠀\n");
        printf("⠀⠀⠀⠀⠀⠀⠙⠯⣛⠭⣻⠶⣬⣉⣛⠛⠃⠿⠿⠃⠿⠃⠚⣀⣁⣤⣾⣿⡀⠀\n");
        printf("⠀⠀⠀⠀⠀⠀⠀⠀⠀⠙⠒⠯⣶⣋⡽⢛⣿⣯⣿⣭⣭⡿⢿⣿⣻⣾⢟⣿⡇⠀\n");
        printf("⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠉⠛⠿⠿⣶⣾⣿⣿⣿⣭⣭⣭⣶⣿⡿⠁⠀\n");
        printf("⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠉⠙⠛⠛⠛⠛⠋⠁⠀⠀⠀\n");
    } else if (strcmp(data, "/moai") == 0) {
        printf("⠀⠀⠀⠀⠀⠀⢀⢄⡒⠒⡒⠒⢰⠒⣒⢶⠤⡀⠀⠀⠀\n");
        printf("⠀⠀⠀⠀⠀⡰⡩⠂⠀⠀⠀⠀⠀⠣⡊⠙⣷⢱⠀⠀⠀\n");
        printf("⠀⠀⠀⠀⣰⡑⣀⣂⡠⢀⠀⠀⠄⡀⡌⡃⡽⢽⡆⠀⠀\n");
        printf("⠀⠀⠀⠀⠻⣿⣿⣿⣿⣿⣿⣶⣶⣦⡤⢄⣌⣻⣇⠀⠀\n");
        printf("⠀⠀⠀⠀⠀⣾⣿⠟⠻⣿⡿⠛⠁⠀⢷⡄⣿⢿⢿⠀⠀\n");
        printf("⠀⠀⠀⠀⡘⡠⢀⣠⢲⠃⢁⠀⠀⢠⠖⠛⠓⢸⣼⡄⠀\n");
        printf("⠀⠀⠀⢰⢋⡴⢋⡏⡜⠐⠁⠈⠤⠀⠀⠀⠀⢮⣻⣧⠀\n");
        printf("⠀⣀⠔⠕⠒⠈⠉⠀⠀⠀⠀⠀⠀⠀⠀⡀⠐⢻⡟⢹⡀\n");
        printf("⠰⣷⣶⣤⣤⣄⣀⣀⣴⣦⡆⠀⠀⠀⡠⡂⠀⠈⢷⡴⡇\n");
        printf("⠀⠀⣼⣿⣿⣿⣿⣿⡿⠋⠁⠀⠀⢀⠇⣴⣿⣶⣾⣿⣧\n");
        printf("⠀⢰⣯⣭⣭⣌⣀⣀⠀⠀⠀⠀⠀⠀⠀⠋⢿⣿⣿⣿⣿\n");
        printf("⠀⠀⣿⣿⣿⣿⣿⣿⣯⡿⠔⠀⠀⠀⠀⠀⠈⣿⣿⣿⣿\n");
        printf("⠀⠀⢸⣿⣿⣿⡿⠛⠀⠀⠀⠀⠀⡀⠀⠀⢐⣿⣿⣿⡇\n");
        printf("⠀⠀⠘⣿⣿⣿⣥⣄⣰⣊⣤⣀⣤⣶⣶⣿⣿⣿⣿⣿⠇\n");
        printf("⠀⠀⠀⠀⠉⠛⠛⠛⠛⠻⠿⠿⠿⠿⠿⠿⠿⠿⠛⠁⠀\n");
    } else if (strcmp(data, "/amongus") == 0) {
        printf("⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣠⣤⣤⣤⣤⣤⣶⣦⣤⣄⡀⠀⠀⠀⠀⠀⠀⠀⠀\n");
        printf("⠀⠀⠀⠀⠀⠀⠀⠀⢀⣴⣿⡿⠛⠉⠙⠛⠛⠛⠛⠻⢿⣿⣷⣤⡀⠀⠀⠀⠀⠀ \n");
        printf("⠀⠀⠀⠀⠀⠀⠀⠀⣼⣿⠋⠀⠀⠀⠀⠀⠀⠀⢀⣀⣀⠈⢻⣿⣿⡄⠀⠀⠀⠀ \n");
        printf("⠀⠀⠀⠀⠀⠀⠀⣸⣿⡏⠀⠀⠀⣠⣶⣾⣿⣿⣿⠿⠿⠿⢿⣿⣿⣿⣄⠀⠀⠀ \n");
        printf("⠀⠀⠀⠀⠀⠀⠀⣿⣿⠁⠀⠀⢰⣿⣿⣯⠁⠀⠀⠀⠀⠀⠀⠀⠈⠙⢿⣷⡄⠀ \n");
        printf("⠀⠀⣀⣤⣴⣶⣶⣿⡟⠀⠀⠀⢸⣿⣿⣿⣆⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣿⣷⠀ \n");
        printf("⠀⢰⣿⡟⠋⠉⣹⣿⡇⠀⠀⠀⠘⣿⣿⣿⣿⣷⣦⣤⣤⣤⣶⣶⣶⣶⣿⣿⣿⠀ \n");
        printf("⠀⢸⣿⡇⠀⠀⣿⣿⡇⠀⠀⠀⠀⠹⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠃⠀ \n");
        printf("⠀⣸⣿⡇⠀⠀⣿⣿⡇⠀⠀⠀⠀⠀⠉⠻⠿⣿⣿⣿⣿⡿⠿⠿⠛⢻⣿⡇⠀⠀ \n");
        printf("⠀⣿⣿⠁⠀⠀⣿⣿⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⣿⣧⠀⠀ \n");
        printf("⠀⣿⣿⠀⠀⠀⣿⣿⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⣿⣿⠀⠀ \n");
        printf("⠀⣿⣿⠀⠀⠀⣿⣿⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⣿⣿⠀⠀ \n");
        printf("⠀⢿⣿⡆⠀⠀⣿⣿⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⣿⡇⠀⠀ \n");
        printf("⠀⠸⣿⣧⡀⠀⣿⣿⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣿⣿⠃⠀⠀ \n");
        printf("⠀⠀⠛⢿⣿⣿⣿⣿⣇⠀⠀⠀⠀⠀⣰⣿⣿⣷⣶⣶⣶⣶⠶⠀⢠⣿⣿⠀⠀⠀ \n");
        printf("⠀⠀⠀⠀⠀⠀⠀⣿⣿⠀⠀⠀⠀⠀⣿⣿⡇⠀⣽⣿⡏⠁⠀⠀⢸⣿⡇⠀⠀⠀ \n");
        printf("⠀⠀⠀⠀⠀⠀⠀⣿⣿⠀⠀⠀⠀⠀⣿⣿⡇⠀⢹⣿⡆⠀⠀⠀⣸⣿⠇⠀⠀⠀ \n");
        printf("⠀⠀⠀⠀⠀⠀⠀⢿⣿⣦⣄⣀⣠⣴⣿⣿⠁⠀⠈⠻⣿⣿⣿⣿⡿⠏⠀⠀⠀⠀ \n");
        printf("⠀⠀⠀⠀⠀⠀⠀⠈⠛⠻⠿⠿⠿⠿⠋⠁⠀\n");
    } else if (strcmp(data, "/pony") == 0) {
        printf("⠀⠀⠀⣠⣴⣾⡿⠿⢷⣶⣄⡠⢤⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀\n");
        printf("⠀⠀⣠⡖⠉⠀⠀⠀⠐⠒⠩⠁⢠⢱⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀\n");
        printf("⢀⣠⠝⠁⠀⢀⢀⡤⣖⡄⠉⠀⠘⢸⣀⣠⠖⢳⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀\n");
        printf("⠈⠁⠒⠒⡎⠙⣏⡞⢻⣿⠂⠀⢀⢶⠁⠋⠀⡮⠤⡄⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀\n");
        printf("⠀⠀⠀⣀⠇⠀⠸⢿⣿⠇⢀⠴⠁⢸⠤⠀⠊⠀⡼⠁⣀⣀⣀⠀⠀⠀⠀⠀⠀⠀\n");
        printf("⠀⠀⠀⠘⢤⡂⠀⠈⠁⢉⠖⣀⣴⡇⡰⢤⠠⢾⣴⡿⠿⠿⢿⣿⣷⣄⠀⠀⠀⠀\n");
        printf("⠀⠀⠀⠀⠀⠈⠉⠉⣝⡣⣾⡿⡳⠁⡀⣊⠤⢞⣵⣴⣎⡀⠀⠈⢻⣿⢷⠀⠀⠀\n");
        printf("⠀⠀⠀⠀⠀⠀⣀⣀⡟⢱⡟⢰⠃⠀⠼⠜⢤⡀⠘⡄⣿⣷⡀⠀⠀⢻⡆⠁⠀⠀\n");
        printf("⠀⠀⠀⡠⠒⠉⠀⠀⠀⠀⠣⣸⠀⠀⠀⠀⢙⠄⢀⣧⣿⣿⠁⠀⠀⢸⡇⠀⠀⠀\n");
        printf("⠀⢠⠊⠀⠀⢀⠔⢒⠒⠒⢢⠤⢀⣀⣈⣄⠈⠙⠻⢿⡿⠃⠀⠀⠀⢸⣇⠀⠀⠀\n");
        printf("⠀⠘⠒⠤⠤⠋⠀⡎⠀⠀⡆⠀⠀⠸⠀⠀⡗⡄⢀⡟⣡⡄⠀⠀⠀⢸⣿⡄⠀⠀\n");
        printf("⠀⠀⠀⠀⠀⠀⠀⡇⠀⠀⢇⠀⢠⠃⠀⢀⢳⠁⠸⢾⠁⢇⠀⠀⢀⠀⢏⠉⠂⠀\n");
        printf("⠀⠀⠀⠀⠀⠀⠀⢇⠀⠀⣸⢀⠎⠀⠀⡘⡆⠀⠀⢸⠀⠈⠲⣄⡈⠢⣌⡳⢄⡀\n");
        printf("⠀⠀⠀⠀⠀⠀⠀⠘⠤⠚⠁⠮⠤⠤⠔⢹⣀⣀⣠⠇⠀⠀⠀⠀⠉⠉⠉⠉⠉⠀\n");
    } else if (strcmp(data, "/apple") == 0) {
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠛⠻⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀⠀⠀⠈⠙⠋⠉⠻⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠟⠃⠀⠀⠀⠀⠀⠀⠀⠀⠀⢻⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠟⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡟⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠘⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣃⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣄⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣷⡄⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣽⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢰⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⠿⠃⠈⠉⠻⣿⣿⣿⣿⣿⣿⣿⣻⣿⠀⠀⠀⠀⠀⠀⠀⠀⠀⣆⠀⠀⣾⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⠇⠀⠀⠀⠀⠀⣿⣿⣿⣿⣿⣿⣿⠋⠋⠀⠀⠀⠀⠀⠀⠀⠀⠀⠹⣷⣾⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣇⠀⠀⠀⠀⠀⠈⠛⠛⠿⠿⣿⠟⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢲⣾⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣦⣄⣀⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠘⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⠇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠛⠉⣉⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠰⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⡏⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢻⣿⣿⣿⣿⣿⣿\n");
    } else if (strcmp(data, "/yes") == 0) { 
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠿⠟⣛⣋⣭⣭⡉⢭⣭⣭⣍⡩⢍⣉⠛⡛⠿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⠿⠛⢋⣁⡐⠦⣛⡛⠶⣶⣶⣭⣝⣓⠶⣦⣭⣙⠲⣭⣛⠶⣥⡂⢝⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⢏⡁⠾⣶⣶⣬⡛⢷⣮⡛⢿⣶⣮⣍⣛⠿⣿⣶⣭⡛⠿⣦⡙⢿⣶⣬⠈⢢⢻⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⡇⠈⢙⡛⢶⣝⡻⢿⣶⣍⡻⢷⣮⣝⡻⠿⢿⣶⣭⣛⣛⢷⠦⢍⡳⠬⠙⢷⠁⡀⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⢁⣶⡀⠉⢷⡉⠹⠶⠈⠇⠉⢀⠈⠀⣶⣀⣀⣁⣰⣶⣶⣶⣾⣷⣶⣿⣷⣶⣶⣆⠸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⠇⣾⣿⣿⣷⣦⣍⠪⢻⣿⣶⢰⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣧⠸⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⠋⣘⣫⣭⣭⣭⣭⣍⣓⣀⢿⣿⡈⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣧⠹⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⡏⢰⠙⠿⠟⡛⠛⠻⢿⣿⣿⡘⣿⣷⠸⣿⡿⠟⣋⣭⡙⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡇⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣷⡄⣠⢡⡀⠈⠃⢒⠤⠌⡛⢡⣿⣿⠆⠫⡰⣤⣬⣙⠿⠘⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡇⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⡟⣰⡟⣼⣇⣀⣀⣨⣴⣷⣶⣮⣿⣿⡇⠑⢬⡈⢭⣭⡅⡆⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⢁⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⡿⢰⣿⢡⣿⣿⣿⣿⣿⣿⣿⣿⣿⡟⠟⣥⠙⣪⡙⢸⣿⢣⠇⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⢃⣾⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⠁⣿⣿⣿⣟⣛⠿⣿⣿⣿⣿⣿⠟⡄⣷⣀⡲⠆⠭⡸⡿⢣⣾⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⢋⣵⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣤⡍⠛⢉⡉⠛⣡⡆⠩⠉⡝⠟⡄⣿⣌⠻⡘⢿⡳⡁⢸⣦⣍⣙⣙⣩⣭⣭⣭⣵⣶⠆⣴⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⡀⠀⢙⠠⡑⠆⠥⠐⠐⠠⠀⢷⡱⡘⢷⡬⣎⠻⣜⡂⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⢸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣶⡄⠂⠀⠀⠀⣀⢀⢠⡆⢦⢧⣀⡌⢗⢌⣑⡺⠇⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⡿⠀⠀⠀⡇⢷⠹⣆⠁⢿⠸⡀⣇⢿⡆⢷⣉⠿⡆⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡆⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⡿⠡⢠⢰⡇⡗⡸⡀⠙⣌⣆⢳⠷⡸⣾⡿⣦⠹⠗⢰⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣷⡸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⡇⢡⡈⡟⠠⠁⣷⢹⡄⠸⣿⠌⢿⠧⣻⢿⡌⢛⣡⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠿⠿⠇⠹⠿⠻⢿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⠃⢎⢁⢄⢰⡇⢿⡎⢷⠠⠻⣶⡰⠁⣃⣠⣴⣿⣿⣿⣿⣿⣿⣿⡿⠟⠉⠀⠀⠀⠀⠀⠀⠀⠀⠀⠛⠛⠻⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣆⠈⢿⠸⣾⡇⠎⣿⢸⡇⠝⢠⣴⠆⣿⣿⣿⣿⣿⣿⡿⠟⠋⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠈⠛⠿⢿⣿\n");
        printf("⣿⣿⣿⣷⣄⡑⠉⠻⠄⢛⣠⣶⡾⠈⣿⣼⣿⡿⠿⠛⠋⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣀⣨\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠏⠀⠀⠀⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣀⣀⣀⣠⣤⣶⣶⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣷⣶⣶⣶⣶⣶⣶⣶⣶⣶⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣇⡀⠉⢉⣸⣿⣉⡉⢉⣸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣆⠈⢿⣿⠟⣠⣿⣿⣿⠿⠿⢿⣿⣿⣿⠿⠿⠿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣆⠈⠋⣴⣿⣿⡟⢁⣶⣷⡄⠙⣯⠀⢼⣶⣄⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⢸⣿⣿⣿⡇⠰⣶⣶⣶⣶⡿⣦⣄⣀⠙⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
        printf("⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠟⠀⠘⠛⣿⣿⣷⣀⠙⠛⢟⣡⡇⠘⠻⠿⢀⣼⣟⠀⢹⣿⣿⣿⣿⣿⣿⣿⣿⣿\n");
    }
}