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

// vars
struct termios term, original;
int in_channel = 0;     // 0 = cant send, 1 = can send
char current_channel[67] = ""; // tracks the current channel

// func prototypes
int setup_socket(int *soc, struct sockaddr_in *server);
void setup_select_fds(fd_set *read_fds, int soc, int *max_fd);
int handle_server_message(int soc, Message *soc_msg, char *soc_msg_buf, int *soc_msg_size, char *stdin_msg_buf, int stdin_msg_size);
int handle_user_input(char *c, char *stdin_msg_buf, int *stdin_msg_size);
void build_outgoing_message(Message *stdin_msg, char *stdin_msg_buf, int stdin_msg_size, char *username, int* username_colour);
void send_message_to_server(int soc, char *out_msg_buf);
int command_handler(char *stdin_msg_buf, int stdin_msg_size, int *in_channel, char *current_channel, int soc);
void request_channels(int soc);
void emoji_parser(char *str);
void render_menu();


void handler() { // whats this for? haven't seen it used anywhere....
    tcsetattr(0, TCSANOW, &original);
    exit(0);
}

// Utils
void enable_non_canonical_input() {
    tcgetattr(0, &term);              // Get current settings
    original = term;                  // Save original settings
    term.c_lflag &= ~(ICANON | ECHO); // Disable canonical mode and echo
    term.c_cc[VMIN] = 1;              // Require 1 character to read
    term.c_cc[VTIME] = 0;             // No timeout
    tcsetattr(0, TCSANOW, &term);     // Apply new settings
}

void disable_non_canonical_input() {
    tcsetattr(0, TCSANOW, &original);
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
    printf("DEBUG: Username: '%s'\n", username); 
    enable_non_canonical_input();
    render_menu();

    // send login to server
    Message *login_msg = malloc(sizeof(Message));
    memset(login_msg, 0, sizeof(Message));
    strcpy(login_msg->action, "LOGIN");
    strcpy(login_msg->user, username);
    login_msg->data_size = strlen(username);
    write(soc, login_msg, sizeof(Message));
    free(login_msg);

    fd_set read_fds;
    int max_fd;

    Message *soc_msg = malloc(sizeof(Message));
    char soc_msg_buf[sizeof(Message)];
    int soc_msg_size = 0;

    Message *stdin_msg = malloc(sizeof(Message));
    char stdin_msg_buf[256];
    int stdin_msg_size = 0;

    char out_msg_buf[sizeof(Message)];

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
                int shouldSend = command_handler(stdin_msg_buf, stdin_msg_size, &in_channel, current_channel, soc);
                if (shouldSend == 1) {
                    build_outgoing_message(stdin_msg, stdin_msg_buf, stdin_msg_size, username, &username_colour);
                    
                    // update local channel tracking
                    if (strncmp(stdin_msg_buf, "/join", 5) == 0) {
                        in_channel = 1;
                        strncpy(current_channel, stdin_msg_buf + 6, 31);
                        current_channel[31] = '\0';
                        render_menu();
                        printf("Joined channel: %s", current_channel);
                    } else if (strncmp(stdin_msg_buf, "/create", 7) == 0) {
                        in_channel = 1;
                        strncpy(current_channel, stdin_msg_buf + 8, 31);
                        current_channel[31] = '\0';
                        render_menu();
                        printf("Created channel: %s", current_channel);
                    }
                    
                    memcpy(out_msg_buf, stdin_msg, sizeof(Message)); // send message to the socket
                    send_message_to_server(soc, out_msg_buf);
                    
                    // show sender their own message with emoji parsed
                    // if (strcmp(stdin_msg->action, "SEND") == 0) {
                    //     char display_buf[256];
                    //     strncpy(display_buf, stdin_msg->data, 255);
                    //     display_buf[255] = '\0';
                    //     emoji_parser(display_buf);
                    //     printf("$ [You] %s\n", display_buf);
                    // } BAD IDEA NOPE UNLESS WE MAKE ONLY SERVER SEND MSG AND WIPE CLIENT's
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
    // TODO 
    // fetch_available_channels();  to get an array of channels
    // use ONLINE

    // TODO loop over that array of channels printing out the first 5
    printf("NonFakeChat (3 users)\n");
    printf("exampleChannel (0 users)\n");
    printf("totallyreal (1 users)\n");

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

    // initialize server address
    server->sin_family = AF_INET;
    server->sin_port = htons(51984);
    memset(&server->sin_zero, 0, 8);
    struct addrinfo *ai;

    // get hostname of current device
    char hostname[1024];
    gethostname(hostname, sizeof(hostname));

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
        printf("$ Server is currently offline.\n");
        printf("$ Awaiting connection... \n");

        while (ret == -1) {
            ret = connect(*soc, (struct sockaddr *)server, sizeof(struct sockaddr_in));
            sleep(1);
        }
    }

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
    if (*soc_msg_size < sizeof(Message)) {
        int r = read(soc, soc_msg_buf + *soc_msg_size, sizeof(Message) - *soc_msg_size);
        *soc_msg_size += r;
    }

    if (*soc_msg_size == sizeof(Message)) {
        memcpy(soc_msg, soc_msg_buf, sizeof(Message));

        if (soc_msg->data_size > 0) {
            soc_msg->data[soc_msg->data_size - 1] = '\0';
        }

        // erase the current input
        for (int i = 0; i < stdin_msg_size; i++) {
            printf("\b");
        }

        emoji_parser(soc_msg->data);

        // add username to msg
        if (strlen(soc_msg->user) > 0) {
            if (soc_msg->username_colour > 0) {
                printf(COLOURED_USER_NAME, soc_msg->username_colour, soc_msg->user, soc_msg->data);
            }
            else {
                printf("[%s] %s\n", soc_msg->user, soc_msg->data);
            }
        } else {
            printf("%s\n", soc_msg->data);
        }


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
    } else if (strncmp(stdin_msg_buf, "/color", 6) == 0) {
        int code = get_code_from_colour_name(stdin_msg_buf + 7, stdin_msg_size - 7);
        if (code == -1)
        {
            printf("Invalid color!\n");
        }
        else
        {
            *username_colour = code;
            printf("Colour changed successfully!\n");
        }
    }
    else if (strncmp(stdin_msg_buf, "/friend", 7) == 0)
    {
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
        strncpy(stdin_msg->data, stdin_msg_buf, stdin_msg_size);
        stdin_msg->data[stdin_msg_size - 1] = '\0';
        stdin_msg->data_size = stdin_msg_size;
    }
} 


// sends the Message struct to server socket
// params: 
//      soc - socket fd
//      out_msg_buf - buffer with message
void send_message_to_server(int soc, char *out_msg_buf) {
    int count = 0;
    while (count < sizeof(Message)) {
        int w = write(soc, out_msg_buf + count, sizeof(Message) - count);
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
int command_handler(char *stdin_msg_buf, int stdin_msg_size, int *in_channel, char *current_channel, int soc) {    
    
    // leave command 
    if (strncmp(stdin_msg_buf, "/leave", 6) == 0) {
        *in_channel = 0;
        current_channel[0] = '\0';
        render_menu();
        return 0;  // dont send to server
    }

    // help command
    if (strncmp(stdin_msg_buf, "/help", 5) == 0) {
        printf("$ Available commands:\n");
        printf("$   /channels         - List active channels\n");
        printf("$   /join   <channel> - Join a channel\n");
        printf("$   /create <channel> - Create a channel\n");
        printf("$   /leave            - Leave current channel\n");
        printf("$   /emoji            - Show available emoji\n");
        printf("$   /ascii            - Show ascii art commands\n");
        printf("$   /exit             - Exit the program\n");
        return 0; 
    }

    // channels command
    if (strncmp(stdin_msg_buf, "/channels", 9) == 0) {
        printf("WORK IN PROGRESS\n");
        // TODO request_channels(soc);
        return 0;
    }   

    // let /join and /create even when not in channel
    if (strncmp(stdin_msg_buf, "/join", 5) == 0 || strncmp(stdin_msg_buf, "/create", 7) == 0) {
        return 1;  // allow send to server
    }

    // regular message in main menu
    if (*in_channel == 0) {
        printf("$ Join a channel first! Use /join <channel>\n");
        return 0;  // block the send
    }

    return 1;  // allow the send
}


// requests the list of line channels + user counts for each
// params: soc - socket fd

// READ ME
// 
// so im not sure how you implemented the ONLINE opcode handling on your end but here's how the function handles it:
// 
// so this func expects a Message struct packed with:
//  msg->action = "ONLINE"
//  msg->data = a comma separated list with this format:    channel_name:user_count,channel_name:user_count,...
//                                              example:    "general:5,random:2,memes:0,help:1"
// feel free to change this function if its easier, and lmk if u need any help
void request_channels(int soc) {
    Message *msg = malloc(sizeof(Message));
    char out_buf[sizeof(Message)];
    
    memset(msg, 0, sizeof(Message));
    strcpy(msg->action, "ONLINE");
    msg->data_size = 0;
    
    memcpy(out_buf, msg, sizeof(Message));
    
    int count = 0;
    while (count < sizeof(Message)) {
        int w = write(soc, out_buf + count, sizeof(Message) - count);
        count += w;
    }
    
    free(msg);
}





// replaces emoji codes with unicode characters client side
// params: str - string to parse
void emoji_parser(char *str) {
    char *pos = strstr(str, ":fire:");
    while (pos != NULL) {
        char *rest = pos + 6;  // everything after :fire:
        memmove(pos + 4, rest, strlen(rest) + 1);  // shift rest of string
        pos[0] = 0xF0;  // 🔥 utf-8 bytes
        pos[1] = 0x9F;
        pos[2] = 0x94;
        pos[3] = 0xA5;
        
        // search for next
        pos = strstr(str, ":fire:");
    }
}
