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

struct termios term, original;

void handler()
{
    tcsetattr(0, TCSANOW, &original);
    exit(0);
}

// Utils

void enable_non_canonical_input()
{
    tcgetattr(0, &term);              // Get current settings
    original = term;                  // Save original settings
    term.c_lflag &= ~(ICANON | ECHO); // Disable canonical mode and echo
    term.c_cc[VMIN] = 1;              // Require 1 character to read
    term.c_cc[VTIME] = 0;             // No timeout
    tcsetattr(0, TCSANOW, &term);     // Apply new settings
}

void disable_non_canonical_input()
{
    tcsetattr(0, TCSANOW, &original);
}

void get_username(char *username, int max_len)
{
    printf("Enter Username: ");
    fflush(stdout);

    char input[256];
    if (fgets(input, sizeof(input), stdin) != NULL)
    {

        // delete newline
        input[strcspn(input, "\n")] = 0;
        strncpy(username, input, max_len - 1);
        username[max_len - 1] = '\0';
    }
    else
    {
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

int main()
{
    // create socket
    int soc = socket(AF_INET, SOCK_STREAM, 0);
    if (soc == -1)
    {
        perror("client: socket");
        exit(1);
    }

    // initialize server address
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(51984);
    memset(&server.sin_zero, 0, 8);
    struct addrinfo *ai;

    // get hostname of current device
    char hostname[1024];
    gethostname(hostname, sizeof(hostname));
    // char *hostname = "teach.cs.toronto.edu";

    /* this call declares memory and populates ailist */
    if (getaddrinfo(hostname, NULL, NULL, &ai) != 0)
    {
        perror("getaddrinfo");
        exit(67);
    }
    server.sin_addr = ((struct sockaddr_in *)ai->ai_addr)->sin_addr;

    // free the memory that was allocated by getaddrinfo for this list
    freeaddrinfo(ai);

    int ret = connect(soc, (struct sockaddr *)&server, sizeof(struct sockaddr_in));
    if (ret == -1)
    {
        printf("Server is not running...\n");
        printf("Try connecting later. \n");

        while (ret == -1)
        {
            ret = connect(soc, (struct sockaddr *)&server, sizeof(struct sockaddr_in));
            sleep(1);
        }
    }

    printf("Connected to the server! \n");

    // user login (has to be before canonical input otherwise gets messed up)
    char username[16];
    int username_colour;
    get_username(username, 16);
    username_colour = get_random_username_colour();
    enable_non_canonical_input();

    // send login to server
    Message *login_msg = malloc(sizeof(Message));
    memset(login_msg, 0, sizeof(Message));
    strcpy(login_msg->action, "LOGIN"); // server stores username, skips broadcast & archive
    strcpy(login_msg->user, username);  // base for future features like user accounts
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

    while (1)
    {
        // Clean fds sets
        FD_ZERO(&read_fds);

        // set socket to be read fd
        FD_SET(soc, &read_fds);
        FD_SET(STDIN_FILENO, &read_fds);

        if (soc > STDIN_FILENO)
        {
            max_fd = soc;
        }
        else
        {
            max_fd = STDIN_FILENO;
        }

        // Select between socket input and std input
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) == -1)
        {
            perror("select");
            exit(1);
        }

        // Do the message reading and printing from a socket
        if (FD_ISSET(soc, &read_fds))
        {
            if (soc_msg_size < sizeof(Message))
            {
                int r = read(soc, soc_msg_buf + soc_msg_size, sizeof(Message) - soc_msg_size);
                soc_msg_size += r;
            }

            if (soc_msg_size == sizeof(Message))
            {
                memcpy(soc_msg, soc_msg_buf, sizeof(Message));

                if (soc_msg->data_size > 0)
                {
                    soc_msg->data[soc_msg->data_size - 1] = '\0';
                }

                // erase the current input
                for (int i = 0; i < stdin_msg_size; i++)
                {
                    printf("\b");
                }

                // add username to msg
                if (strlen(soc_msg->user) > 0)
                {
                    if (soc_msg->username_colour > 0)
                    {
                        printf(COLOURED_USER_NAME, soc_msg->username_colour, soc_msg->user, soc_msg->data);
                    }
                    else
                    {
                        printf("[%s] %s\n", soc_msg->user, soc_msg->data);
                    }
                }
                else
                {
                    printf("%s\n", soc_msg->data);
                }

                printf("> ");
                // reprint the current input after printing the message
                for (int i = 0; i < stdin_msg_size; i++)
                {
                    printf("%c", stdin_msg_buf[i]);
                }
                fflush(stdout);

                // Clean up the message and the buffer
                soc_msg_size = 0;
            }
        }

        // Do message reading and sending from the input
        if (FD_ISSET(STDIN_FILENO, &read_fds))
        {
            char c = getchar();
            if (c == 127 || c == '\b')
            {
                // erase on backspace
                if (stdin_msg_size > 0)
                {
                    printf("\b \b");
                    fflush(stdout);
                    stdin_msg_size--;
                }
            }
            else
            {
                stdin_msg_buf[stdin_msg_size] = c;
                stdin_msg_size += 1;
                printf("%c", c);
                fflush(stdout);
            }

            if (c == '\n' || stdin_msg_size == 256)
            {

                // empty message moment
                if (stdin_msg_size <= 1 && c == '\n')
                {
                    printf("> ");
                    fflush(stdout);
                    stdin_msg_size = 0;
                    continue;
                }

                if (strncmp(stdin_msg_buf, ":JOIN", 5) == 0)
                {
                    memset(stdin_msg, 0, sizeof(Message)); // remove garbage
                    strcpy(stdin_msg->action, "JOIN");
                    strcpy(stdin_msg->user, username);
                    strncpy(stdin_msg->data, stdin_msg_buf + 6, stdin_msg_size - 6);
                    stdin_msg->data[stdin_msg_size - 7] = '\0';
                    stdin_msg->data_size = stdin_msg_size - 6;
                }
                else if (strncmp(stdin_msg_buf, ":START", 6) == 0)
                {
                    memset(stdin_msg, 0, sizeof(Message)); // remove garbage
                    strcpy(stdin_msg->action, "START");
                    strcpy(stdin_msg->user, username);
                    strncpy(stdin_msg->data, stdin_msg_buf + 7, stdin_msg_size - 7);
                    stdin_msg->data[stdin_msg_size - 8] = '\0';
                    stdin_msg->data_size = stdin_msg_size - 7;
                }
                else if (strncmp(stdin_msg_buf, ":COLOR", 6) == 0)
                {
                    int code = get_code_from_colour_name(stdin_msg_buf + 7, stdin_msg_size - 7);
                    if (code == -1)
                    {
                        printf("Invalid color!\n");
                    }
                    else
                    {
                        username_colour = code;
                        printf("Colour changed successfully!\n");
                    }
                }
                else if (strncmp(stdin_msg_buf, ":FRIEND", 7) == 0)
                {
                    memset(stdin_msg, 0, sizeof(Message)); // remove garbage
                    strcpy(stdin_msg->action, "FRIEND");
                    strcpy(stdin_msg->user, username);
                    strncpy(stdin_msg->data, stdin_msg_buf + 8, stdin_msg_size - 8);
                    stdin_msg->data[stdin_msg_size - 9] = '\0';
                    stdin_msg->data_size = stdin_msg_size - 8;
                }
                else
                {
                    memset(stdin_msg, 0, sizeof(Message)); // remove garbage
                    strcpy(stdin_msg->action, "SEND");
                    strcpy(stdin_msg->user, username);
                    stdin_msg->username_colour = username_colour;
                    strncpy(stdin_msg->data, stdin_msg_buf, stdin_msg_size);
                    stdin_msg->data[strlen(stdin_msg_buf) - 1] = '\0';
                    stdin_msg->data_size = stdin_msg_size;
                }

                // Send message to  the socket
                memcpy(out_msg_buf, stdin_msg, sizeof(Message));

                int count = 0;
                while (count < sizeof(Message))
                {
                    int w = write(soc, out_msg_buf + count, sizeof(Message) - count);
                    count += w;
                }

                // Clean up the message and buffer
                stdin_msg_size = 0;
                memset(stdin_msg_buf, 0, 256); // clean AFTER sending too

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
