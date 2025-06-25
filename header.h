#pragma once

#pragma pack(push, 1)
typedef struct PacketHeader {
    uint8_t type;
    uint16_t length; // Length of the data in bytes
} PacketHeader;
#pragma pack(pop)

typedef enum PacketType {
	TYPE_TEXT = 0, // Data : message string
	TYPE_NICK = 1, // Data : new nickname string
    TYPE_ROOMS = 2,
	TYPE_CREATE = 3, // Data : room name string
	TYPE_JOIN = 4, // Data : room name string
    TYPE_LEAVE = 5,
	TYPE_USERS = 6, // Data : list of users in the room
	TYPE_FRIENDS = 7, // Data : list of friends
    TYPE_CHANGE = 8,
    TYPE_KICK = 9, // Data : nickname of the user to kick
    TYPE_ADD_FRIEND = 11, // Data : nickname of the user to add
	TYPE_REMOVE_FRIEND = 12, // Data : nickname of the user to remove
	TYPE_VOTES = 13, // Data : vote title and options
	TYPE_CREATE_VOTE = 14, // Data : vote title and options
	TYPE_DO_VOTE = 15, // Data : vote name and selected option
	TYPE_VOTE_RESULT = 16,
	TYPE_ERROR = 17, // Data : error message string
} PacketType;

typedef struct Client {
    int sock;
    char nick[64];
    pthread_t thread;
    struct Room* room;
    struct Client* next;
    struct Client* prev;
    struct Client* room_next;
    struct Client* room_prev;
    int friend_count; // Number of friends
	struct Client* friend_list[20]; // List of friends
} Client;

typedef struct Room {
    char name[64];
    unsigned int id; // Unique ID for the room
    struct Client* members;
    int member_count;
    struct Room* next;
    struct Room* prev;
    struct Client* owner; // owner of the room
	struct Vote* votes[20]; // List of votes in the room
    int num_votes;
} Room;

typedef struct Vote {
	char title[64]; // Title of vote
	struct Client* voters[20]; // List of voters
	char* options[10][64]; // Options for the vote
	int option_votes[10]; // Number of votes for each option
    int num_options; // Number of options
	int is_anonymous; // Whether the vote is anonymous
	int is_duplicate; // Whether the vote allows duplicate votes
	int is_finished; // Whether the vote is finished
} Vote;
