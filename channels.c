#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h> /* Internet domain header */
#include <arpa/inet.h>  /* only needed on my mac */
#include <dirent.h>     /* for directory operations */
#include "entities.h"
#include "channels.h"
#include "set_ops.h"
#include "constants.h"

// Return 0 if channel does not exist and 1 if it does
int check_channel_exists(Channel **channels, int num_channels, char *channel_name)
{
    for (int i = 0; i < num_channels; i++)
    {
        if (strcmp(channels[i]->name, channel_name) == 0)
        {
            return 1;
        }
    }
    return 0;
}

void handle_create_channel(Channel **channels, int *num_channels, char *channel_name)
{
    Channel *new_channel = malloc(sizeof(Channel));
    strcpy(new_channel->name, channel_name);
    channels[*num_channels] = new_channel;
    *num_channels += 1;
}

void handle_join_channel(Client *client, Channel **channels, int num_channels, char *channel_name, int client_soc_index)
{
    for (int i = 0; i < num_channels; i++)
    {
        if (strcmp(channels[i]->name, channel_name) == 0)
        {
            Channel *channel = channels[i];
            SET_BIT(channel->active_members, client_soc_index);
            if (client->active_channel != NULL)
            {
                CLEAR_BIT(client->active_channel->active_members, client_soc_index);
            }
            client->active_channel = channel;
            channel->num_members += 1;
        }
    }
}

Channel *find_channel_by_name(Channel **channels, int num_channels, char *channel_name)
{
    for (int i = 0; i < num_channels; i++)
    {
        if (strcmp(channel_name, channels[i]->name) == 0)
        {
            return channels[i];
        }
    }
    return NULL;
}

void load_channels(Channel **channels, int *num_channels)
{
    DIR *directory = opendir(CHANNEL_DIR);
    if (directory == NULL)
    {
        perror("opendir");
        return;
    }
    struct dirent *dp;
    while ((dp = readdir(directory)) != NULL)
    {
        if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
        {
            continue;
        }
        char channel_name[32];
        strncpy(channel_name, dp->d_name, strlen(dp->d_name) - 4);
        channel_name[strlen(dp->d_name) - 4] = '\0';
        handle_create_channel(channels, num_channels, channel_name);
    }
    closedir(directory);
}