#pragma once

// General structs
#pragma pack(push, 1) // remove padding
typedef struct message
{
	char action[8];
	int data_size;
	char user[16];
	int username_colour; // id of the colour that username should be displayed in
	char channel[32];
	char data[256];
	int timestamp;
} Message;
#pragma pack(pop)

// Struct to make a doubly-linked list of messages
typedef struct message_list
{
	Message *message;
	struct message_list *next;
	struct message_list *prev;
} MessageList;

typedef struct channel
{
	char name[32];
	uint8_t active_members[1024 / 8]; // bit map that indicates who is currently in the channel
	int num_members;
} Channel;

typedef struct private_channel
{
	Channel *channel;
	char invited_users[128][16]; // limit number of users in private channels to 128
	int num_invited_users;
} PrivateChannel;

typedef struct user
{
	char name[16];
	Channel *channels;
} User;

typedef struct server
{
	Channel *channels;
} Server;

// Server structs
typedef struct client
{
	int soc;
	char user_name[16];
	Channel *active_channel;
	char in_buf[16384];
	char out_buf[16384];
	int in_buf_size;
	int out_buf_size;
} Client;

typedef struct message_queue
{
	MessageList *head;
	MessageList *tail;
} MessageQueue;
