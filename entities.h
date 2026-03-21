// General structs

typedef struct message {
	char action[8];
	int data_size;
	char user[16];
	char channel[32];
	char data[256];
	int timestamp;
} Message;

// Struct to make a doubly-linked list of messages
typedef struct message_list {
	Message *message;
	struct message_list *next ;
	struct message_list *prev;
} MessageList;

typedef struct channel {
	char name[32];
	struct channel *members;
	MessageList* messages_head;
	MessageList* messages_tail;
} Channel;

typedef struct user {
	char name[16];
	Channel *channels;
} User;

typedef struct server {
	Channel *channels;
} Server;

// Server structs
typedef struct client {
	int soc;
	char user_name[16];
	char in_buf[384];
	char out_buf[1536];
	int in_buf_size;
	int out_buf_size;
} Client;


typedef struct message_queue{
	MessageList* head;
	MessageList* tail;
} MessageQueue;

