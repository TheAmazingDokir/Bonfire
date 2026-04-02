#include "entities.h"

// func prototypes
int check_channel_exists(Channel **channels, int num_channels, char *channel_name);
Channel *handle_create_channel(Channel **channels, int *num_channels, char *channel_name);
void handle_join_channel(Client *client, Channel **channels, int num_channels, char *channel_name, int client_soc_index);
void load_channels(Channel **channels, int *num_channels);
Channel *find_channel_by_name(Channel **channels, int num_channels, char *channel_name);
int check_is_private_channel(PrivateChannel **private_channels, int num_private_channels, char *channel_name);
void handle_create_private_channel(Client *client, PrivateChannel **private_channels, int *num_private_channels, Channel *channel);
void save_private_channels(PrivateChannel **private_channels, int num_private_channels);
void load_private_channels(PrivateChannel **private_channels, int *num_private_channels, Channel **channels, int num_channels);
void handle_invite_to_private_channel(PrivateChannel **channels, int num_channels, char *channel_name, char *username);
char *check_if_friend_channel(Client *client, PrivateChannel **channels, int num_channels, char *channel_name);
int check_client_allowed_to_join(Client *client, PrivateChannel **private_channels, int num_private_channels, char *channel_name);