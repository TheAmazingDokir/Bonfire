#include "entities.h"

// func prototypes
int check_channel_exists(Channel **channels, int num_channels, char *channel_name);
void handle_create_channel(Channel **channels, int *num_channels, char *channel_name);
void handle_join_channel(Client *client, Channel **channels, int num_channels, char *channel_name, int client_soc_index);
void load_channels(Channel **channels, int *num_channels);
Channel *find_channel_by_name(Channel **channels, int num_channels, char *channel_name);