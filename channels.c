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

// Returns pointer to created channel
Channel *handle_create_channel(Channel **channels, int *num_channels, char *channel_name)
{
    Channel *new_channel = malloc(sizeof(Channel));
    strcpy(new_channel->name, channel_name);
    channels[*num_channels] = new_channel;
    *num_channels += 1;
    return new_channel;
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

// Private channels utils

// Return 0 if channel is not private and 1 if it is
int check_is_private_channel(PrivateChannel **private_channels, int num_private_channels, char *channel_name)
{
    for (int i = 0; i < num_private_channels; i++)
    {
        if (strcmp(private_channels[i]->channel->name, channel_name) == 0)
        {
            return 1;
        }
    }
    return 0;
}

void handle_create_private_channel(Client *client, PrivateChannel **private_channels, int *num_private_channels, Channel *channel)
{
    PrivateChannel *new_private_channel = malloc(sizeof(PrivateChannel));
    new_private_channel->channel = channel;
    strncpy(new_private_channel->invited_users[0], client->user_name, 16);
    new_private_channel->num_invited_users = 1;
    private_channels[*num_private_channels] = new_private_channel;
    *num_private_channels += 1;
    save_private_channels(private_channels, *num_private_channels);
}

// For purposes of keeping the feature simple we are using save all / load all functions instead of partial updates
void save_private_channels(PrivateChannel **private_channels, int num_private_channels)
{
    FILE *f = fopen(PRIVATE_CHANNELS_FILE, "wb");
    if (f == NULL)
    {
        perror("fopen save private channels");
        return;
    }
    fwrite(&num_private_channels, sizeof(int), 1, f);
    for (int i = 0; i < num_private_channels; i++)
    {
        fwrite(private_channels[i]->channel->name, sizeof(char), 32, f);
        fwrite(&private_channels[i]->num_invited_users, sizeof(int), 1, f);
        for (int j = 0; j < private_channels[i]->num_invited_users; j++)
        {
            fwrite(private_channels[i]->invited_users[j], sizeof(char), 16, f);
        }
    }
    fclose(f);
}

// assumes private channels is a static array
void load_private_channels(PrivateChannel **private_channels, int *num_private_channels, Channel **channels, int num_channels)
{
    FILE *f = fopen(PRIVATE_CHANNELS_FILE, "rb");
    if (f == NULL)
    {
        perror("fopen load private channels");
        return;
    }
    fread(num_private_channels, sizeof(int), 1, f);
    for (int i = 0; i < *num_private_channels; i++)
    {
        char channel_name[32];
        fread(channel_name, sizeof(char), 32, f);
        Channel *channel = find_channel_by_name(channels, num_channels, channel_name);

        PrivateChannel *private_channel = malloc(sizeof(PrivateChannel));

        // Since find_channel_by_name returns NULL if not found we do not need an explicit handling here
        // This will only occur if friendship request was sent but nobody wrote anything to the chat
        private_channel->channel = channel;
        fread(&private_channel->num_invited_users, sizeof(int), 1, f);
        for (int j = 0; j < private_channel->num_invited_users; j++)
        {
            fread(private_channel->invited_users[j], sizeof(char), 16, f);
        }
        private_channels[i] = private_channel;
    }
    fclose(f);
}

void handle_invite_to_private_channel(PrivateChannel **channels, int num_channels, char *channel_name, char *username)
{
    for (int i = 0; i < num_channels; i++)
    {
        if (strcmp(channels[i]->channel->name, channel_name) == 0)
        {
            strncpy(channels[i]->invited_users[channels[i]->num_invited_users], username, 16);
            channels[i]->num_invited_users += 1;
        }
    }
    save_private_channels(channels, num_channels);
}

// Returns a friend channel name if exists and NULL if does not
char *check_if_friend_channel(Client *client, PrivateChannel **channels, int num_channels, char *channel_name)
{
    char *friend_channel_name_1 = malloc(32 * sizeof(char));
    char *friend_channel_name_2 = malloc(32 * sizeof(char));
    strcpy(friend_channel_name_1, channel_name);
    strcat(friend_channel_name_1, client->user_name);

    strcpy(friend_channel_name_2, client->user_name);
    strcat(friend_channel_name_2, channel_name);

    for (int i = 0; i < num_channels; i++)
    {
        if (strcmp(channels[i]->channel->name, friend_channel_name_1) == 0)
        {
            free(friend_channel_name_2);
            return friend_channel_name_1;
        }
        if (strcmp(channels[i]->channel->name, friend_channel_name_2) == 0)
        {
            free(friend_channel_name_1);
            return friend_channel_name_2;
        }
    }
    free(friend_channel_name_1);
    free(friend_channel_name_2);
    return NULL;
}

// Returns 1 if allowed 0 if they are not allowed
int check_client_allowed_to_join(Client *client, PrivateChannel **private_channels, int num_private_channels, char *channel_name)
{
    if (check_is_private_channel(private_channels, num_private_channels, channel_name) == 0)
    {
        return 1;
    }

    for (int i = 0; i < num_private_channels; i++)
    {
        if (strcmp(private_channels[i]->channel->name, channel_name) == 0)
        {
            PrivateChannel *channel = private_channels[i];
            for (int i = 0; i < channel->num_invited_users; i++)
            {
                if (strcmp(channel->invited_users[i], client->user_name) == 0)
                {
                    return 1;
                }
            }
        }
    }
    return 0;
}