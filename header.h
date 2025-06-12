#pragma once

typedef struct PacketHeader {
    uint8_t type;
    uint16_t length; // Length of the data in bytes
} PacketHeader;

typedef enum PacketType {
    TYPE_TEXT = 0,
    TYPE_NICK = 1,
    TYPE_ROOMS = 2,
    TYPE_CREATE = 3,
    TYPE_JOIN = 4,
    TYPE_LEAVE = 5,
    TYPE_USERS = 6,
    TYPE_CHANGE = 7,
    TYPE_KICK = 8,
    TYPE_DM = 9,
    TYPE_HELP = 10,
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
} Client;

typedef struct Room {
    char name[64];
    unsigned int id; // Unique ID for the room
    struct Client* members;
    int member_count;
    struct Room* next;
    struct Room* prev;
    struct Client* owner; // owner of the room
} Room;
