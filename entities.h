typedef struct message {
	char action[8];
	char user[16];
	char data[256];
	int timestamp;
} Message

typedef struct user {
	char name[16];
	Channel *channels;
} User

typedef struct server {
	Channel *channels;
} Server

typedef struct channel {
	char name[32];
	User* members;
	Message* messages_head;
	Message* messages_tail;
}

