#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h> // This was incomplete in the diff: fcntl. -> fcntl.h
#include <sys/epoll.h>
#include <pthread.h>
#include <errno.h> // For errno
#include <stdarg.h> // For va_list, va_start, va_end
#include <sys/time.h> // For struct timeval
#include <time.h> // For time()
#include "header.h"

#define PORTNUM 9000
#define BUFFER_SIZE 1024
#define MAX_EVENTS 15

// Forward declarations
typedef struct Client Client;
typedef struct Room Room;
void *client_process(void *arg);
ssize_t safe_send(int sock, const char *msg, PacketType type);
void broadcast_room(Room *room, Client *sender, const char *format, ...);
void broadcast_lobby(Client *sender, const char *format, ...);
void list_add_client_unlocked(Client *cli);
void list_remove_client_unlocked(Client *cli);
Client *find_client_by_sock_unlocked(int sock);
Client *find_client_by_nick_unlocked(const char *nick);
void list_add_room_unlocked(Room *room);
void list_remove_room_unlocked(Room *room);
Room *find_room_unlocked(const char *name);
Room *find_room_by_id_unlocked(unsigned int id);
Room *find_room_by_id(unsigned int id);
void room_add_member_unlocked(Room *room, Client *cli);
void room_remove_member_unlocked(Room *room, Client *cli);
void destroy_room_if_empty_unlocked(Room *room);
void process_server_command(int epfd, int server_sock);

Client *g_clients = NULL;
Room *g_rooms = NULL;
static unsigned int g_next_room_id = 1; // For assigning unique room IDs

// Mutexes for thread-safe list access
pthread_mutex_t g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_rooms_mutex = PTHREAD_MUTEX_INITIALIZER; // Protects global room list AND room member lists

// NULL 상태의 친구 Client를 찾아 친구 목록에서 제거
void clean_friend_list(Client* cli)
{
    for (int i = 0; i < cli->friend_count; i++) {
        if (cli->friend_list[i] == NULL) {
            // Shift remaining friends down
            for (int j = i; j < cli->friend_count - 1; j++) {
                cli->friend_list[j] = cli->friend_list[j + 1];
            }
            cli->friend_list[--cli->friend_count] = NULL; // Decrease count and nullify last element
            i--; // Stay at the same index to check the next friend
        }
    }
}

void list_remove_friend_unlocked(Client* cli, Client* friend_cli) {
	for (int i = 0; i < cli->friend_count; i++) {
		if (cli->friend_list[i] == friend_cli) {
			// Shift remaining friends down
			for (int j = i; j < cli->friend_count - 1; j++) {
				cli->friend_list[j] = cli->friend_list[j + 1];
			}
			cli->friend_list[--cli->friend_count] = NULL; // Decrease count and nullify last element
			return;
		}
	}
	printf("[ERROR] Friend not found in list.\n");
}

void list_add_friend_unlocked(Client* cli, Client* friend_cli) {
	if (cli->friend_count < 20) { // Assuming max 20 friends
		cli->friend_list[cli->friend_count++] = friend_cli;
	}
	else {
		printf("[ERROR] Cannot add more friends, limit reached.\n");
	}
    clean_friend_list(cli);
}

void list_add_client_unlocked(Client *cli) {
    if (g_clients == NULL) {
        g_clients = cli;
        cli->next = NULL;
        cli->prev = NULL;
    } else {
        Client *current = g_clients;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = cli;
        cli->prev = current;
        cli->next = NULL;
    }
}

void list_remove_client_unlocked(Client *cli) {
    if (cli->prev != NULL) {
        cli->prev->next = cli->next;
    } else {
        g_clients = cli->next;
    }
    if (cli->next != NULL) {
        cli->next->prev = cli->prev;
    }
    cli->prev = NULL;
    cli->next = NULL;
}

Client *find_client_by_sock_unlocked(int sock) {
    Client *current = g_clients;
    while (current != NULL) {
        if (current->sock == sock) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

Client *find_client_by_nick_unlocked(const char *nick) {
    Client *current = g_clients;
    while (current != NULL) {
        if (strcmp(current->nick, nick) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

void list_add_room_unlocked(Room *room) {
    if (g_rooms == NULL) {
        g_rooms = room;
        room->next = NULL;
        room->prev = NULL;
    } else {
        Room *current = g_rooms;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = room;
        room->prev = current;
        room->next = NULL;
    }
}

void list_remove_room_unlocked(Room *room) {
    if (room->prev != NULL) {
        room->prev->next = room->next;
    } else {
        g_rooms = room->next;
    }
    if (room->next != NULL) {
        room->next->prev = room->prev;
    }
    room->prev = NULL;
    room->next = NULL;
}

Room *find_room_unlocked(const char *name) {
    Room *current = g_rooms;
    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// Find room by ID (caller must hold g_rooms_mutex)
Room *find_room_by_id_unlocked(unsigned int id) {
    Room *current = g_rooms;
    while (current != NULL) {
        if (current->id == id) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

void room_add_member_unlocked(Room *room, Client *cli) {
    if (room->members == NULL) {
        room->members = cli;
        cli->room_next = NULL;
        cli->room_prev = NULL;
    } else {
        Client *current = room->members;
        while (current->room_next != NULL) {
            current = current->room_next;
        }
        current->room_next = cli;
        cli->room_prev = current;
        cli->room_next = NULL;
    }
    cli->room = room;
    room->member_count++;
}

void room_remove_member_unlocked(Room *room, Client *cli) {
    if (cli->room_prev != NULL) {
        cli->room_prev->room_next = cli->room_next;
    } else {
        room->members = cli->room_next;
    }
    if (cli->room_next != NULL) {
        cli->room_next->room_prev = cli->room_prev;
    }
    cli->room_prev = NULL;
    cli->room_next = NULL;
    cli->room = NULL; // Important: Clear client's room pointer
    room->member_count--;
}

void destroy_room_if_empty_unlocked(Room *room) {
    if (room->member_count <= 0) {
        printf("[INFO] Room '%s' is empty, destroying.\n", room->name);
        list_remove_room_unlocked(room);
        free(room);
    }
}

void list_add_friend(Client* cli, Client* friend_cli) {
	pthread_mutex_lock(&g_clients_mutex);
	list_add_friend_unlocked(cli, friend_cli);
	pthread_mutex_unlock(&g_clients_mutex);
}

void list_remove_friend(Client* cli, Client* friend_cli) {
	pthread_mutex_lock(&g_clients_mutex);
	list_remove_friend_unlocked(cli, friend_cli);
	pthread_mutex_unlock(&g_clients_mutex);
}

void list_add_client(Client *cli) {
    pthread_mutex_lock(&g_clients_mutex);
    list_add_client_unlocked(cli);
    pthread_mutex_unlock(&g_clients_mutex);
}

void list_remove_client(Client *cli) {
    pthread_mutex_lock(&g_clients_mutex);
    list_remove_client_unlocked(cli);
    pthread_mutex_unlock(&g_clients_mutex);
}

Client *find_client_by_sock(int sock) {
    pthread_mutex_lock(&g_clients_mutex);
    Client *cli = find_client_by_sock_unlocked(sock);
    pthread_mutex_unlock(&g_clients_mutex);
    return cli;
}

Client *find_client_by_nick(const char *nick) {
    pthread_mutex_lock(&g_clients_mutex);
    Client *cli = find_client_by_nick_unlocked(nick);
    pthread_mutex_unlock(&g_clients_mutex);
    return cli;
}

void list_add_room(Room *room) {
    pthread_mutex_lock(&g_rooms_mutex);
    list_add_room_unlocked(room);
    pthread_mutex_unlock(&g_rooms_mutex);
}

Room *find_room(const char *name) {
    pthread_mutex_lock(&g_rooms_mutex);
    Room *room = find_room_unlocked(name);
    pthread_mutex_unlock(&g_rooms_mutex);
    return room;
}

Room *find_room_by_id(unsigned int id) {
    pthread_mutex_lock(&g_rooms_mutex);
    Room *room = find_room_by_id_unlocked(id);
    pthread_mutex_unlock(&g_rooms_mutex);
    return room;
}


void room_add_member(Room *room, Client *cli) {
    pthread_mutex_lock(&g_rooms_mutex);
    room_add_member_unlocked(room, cli);
    pthread_mutex_unlock(&g_rooms_mutex);
}

void room_remove_member(Room *room, Client *cli) {
    pthread_mutex_lock(&g_rooms_mutex);
    room_remove_member_unlocked(room, cli);
    pthread_mutex_unlock(&g_rooms_mutex);
}

void destroy_room_if_empty(Room *room) {
    pthread_mutex_lock(&g_rooms_mutex);
    destroy_room_if_empty_unlocked(room);
    pthread_mutex_unlock(&g_rooms_mutex);
}

ssize_t safe_send(int sock, const char *msg, PacketType type) {
    if (sock < 0 || msg == NULL) return -1;
    ssize_t len = strlen(msg);
    // 패킷 헤더 부착
	PacketHeader header;
	header.type = type;
	header.length = len + 1; // +1 for null terminator
	char buffer[BUFFER_SIZE];
	memcpy(buffer, &header, sizeof(PacketHeader));
	if (len >= BUFFER_SIZE - sizeof(PacketHeader)) {
		fprintf(stderr, "[ERROR] Message too long to send.\n");
		return -1; // Message too long
	}
	snprintf(buffer + sizeof(PacketHeader), BUFFER_SIZE - sizeof(PacketHeader), "%s\n", msg);

	// Send the message in a loop to ensure all data is sent
    ssize_t total_sent = 0;
	ssize_t bytes_left = len + sizeof(PacketHeader) + 1; // +1 for null terminator

    while (bytes_left > 0) {
        ssize_t sent = send(sock, buffer + total_sent, bytes_left, 0);
        if (sent < 0) {
            // For blocking sockets, EAGAIN/EWOULDBLOCK is unlikely unless a timeout is set.
            // Any send error here is typically a more serious issue.
            perror("send error");
            return -1;
        }
        total_sent += sent;
        bytes_left -= sent;
    }
    return total_sent;
}

void broadcast_room(Room *room, Client *sender, const char *format, ...) {
    if (!room) return; // Cannot broadcast to a null room

    char message[BUFFER_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    pthread_mutex_lock(&g_rooms_mutex);
    Client *member = room->members;
    while (member != NULL) {
        // A more robust check might involve a flag in the Client struct for validity
        if (/*member != sender && */member->sock >= 0) {
            safe_send(member->sock, message, TYPE_TEXT);
        }
        member = member->room_next;
    }
    pthread_mutex_unlock(&g_rooms_mutex);
}

void broadcast_lobby(Client *sender, const char *format, ...) {
    char message[BUFFER_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    pthread_mutex_lock(&g_clients_mutex);
    Client *client = g_clients;
    while (client != NULL) {
        if (client != sender && client->room == NULL && client->sock >= 0) {
             safe_send(client->sock, message, TYPE_TEXT);
        }
        client = client->next;
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

void cmd_users(Client *cli) {
    char user_list[BUFFER_SIZE];
    user_list[0] = '\0';

    if (cli->room) {
        strcat(user_list, "[Server] Users in room '");
        strcat(user_list, cli->room->name);
        strcat(user_list, "': ");

        pthread_mutex_lock(&g_rooms_mutex);
        Client *member = cli->room->members;
        while (member != NULL) {
            strcat(user_list, member->nick);
            if (member->room_next != NULL) {
                strcat(user_list, ", ");
            }
            member = member->room_next;
        }
        pthread_mutex_unlock(&g_rooms_mutex);
        strcat(user_list, "\n");
        safe_send(cli->sock, user_list, TYPE_USERS);

    } else {
        strcat(user_list, "[Server] Connected users: ");
        pthread_mutex_lock(&g_clients_mutex);
        Client *client = g_clients;
        while (client != NULL) {
            strcat(user_list, client->nick);
            if (client->next != NULL) {
                strcat(user_list, ", ");
            }
            client = client->next;
        }
        pthread_mutex_unlock(&g_clients_mutex);
        strcat(user_list, "\n");
        safe_send(cli->sock, user_list, TYPE_USERS);
    }
}

void cmd_kick(Client* cli, const char* target_nick) {
	if (!target_nick || strlen(target_nick) == 0) {
		safe_send(cli->sock, "[Server] Usage: /kick <nickname>\n", TYPE_ERROR);
		return;
	}
	// Check if the client is in a room
	if (cli->room == NULL) {
		safe_send(cli->sock, "[Server] You must be in a room to kick users.\n", TYPE_ERROR);
		return;
	}
	// Check if the client is the owner of the room
	if (cli->room->owner != cli) {
		safe_send(cli->sock, "[Server] Only the room owner can kick users.\n", TYPE_ERROR);
		return;
	}
	// Find the target client
	Client* target_cli = find_client_by_nick(target_nick);
	if (!target_cli || target_cli->room != cli->room) {
		char error_msg[BUFFER_SIZE];
		snprintf(error_msg, sizeof(error_msg), "[Server] User '%s' not found in this room.\n", target_nick);
		safe_send(cli->sock, error_msg, TYPE_ERROR);
		return;
	}
	// Remove the target client from the room
	room_remove_member(cli->room, target_cli);
	printf("[INFO] Client %s kicked %s from room '%s'.\n", cli->nick, target_nick, cli->room->name);
	char success_msg[BUFFER_SIZE];
	snprintf(success_msg, sizeof(success_msg), "[Server] User '%s' has been kicked from the room.\n", target_nick);
	// safe_send(cli->sock, success_msg);
	broadcast_room(cli->room, NULL, "[Server] %s has been kicked from the room by %s.\n", target_nick, cli->nick);
}

void cmd_change(Client* cli, const char* room_name) {
	// 클라이언트가 위치한 방 확인
    Room* current_room = cli->room;
	if (current_room == NULL) {
		safe_send(cli->sock, "[Server] You are not in any room. Please join a room first.\n", TYPE_ERROR);
		return;
	}
    // 클라이언트가 방장인지 확인
	if (current_room->owner != cli) {
		safe_send(cli->sock, "[Server] Only the room owner can change the room name.\n", TYPE_ERROR);
		return;
	}
	// 방 이름이 유효한지 확인
    if (!room_name || strlen(room_name) == 0) {
		safe_send(cli->sock, "[Server] Usage: /change <new_room_name>\n", TYPE_ERROR);
    }
	// 방 이름이 중복되는지 확인
	pthread_mutex_lock(&g_rooms_mutex);
	Room* existing_room = find_room_unlocked(room_name);
	if (existing_room != NULL) {
		pthread_mutex_unlock(&g_rooms_mutex);
		char error_msg[BUFFER_SIZE];
		snprintf(error_msg, sizeof(error_msg), "[Server] Room name '%s' already exists.\n", room_name);
		safe_send(cli->sock, error_msg, TYPE_ERROR);
		return;
	}
	// 방 이름 변경
	strncpy(current_room->name, room_name, sizeof(current_room->name) - 1);
	current_room->name[sizeof(current_room->name) - 1] = '\0';
	pthread_mutex_unlock(&g_rooms_mutex);
	char success_msg[BUFFER_SIZE];
	snprintf(success_msg, sizeof(success_msg), "[Server] Room name changed to '%s'.\n", current_room->name);
	// safe_send(cli->sock, success_msg);
	broadcast_room(current_room, cli, "[Server] The room name has been changed to '%s'.\n", current_room->name);
}

void cmd_rooms(int sock) {
    pthread_mutex_lock(&g_rooms_mutex);
    char room_list[BUFFER_SIZE] = { 0 }; // 충분히 큰 버퍼를 준비
    strcpy(room_list, "Rooms:\n");
	printf("[DEBUG] Rooms:\n");
	Room* current = g_rooms;
	while (current != NULL) {
		char room_info[BUFFER_SIZE];
		snprintf(room_info, sizeof(room_info), "%s %d\n", current->name, current->member_count);
		printf("[DEBUG] %s", room_info);
		strncat(room_list, room_info, sizeof(room_list) - strlen(room_list) - 1); // 안전하게 추가
		current = current->next;
	}
    pthread_mutex_unlock(&g_rooms_mutex);
    safe_send(sock, room_list, TYPE_ROOMS);
}

void cmd_create_room(Client *cli, const char *room_name) {
    if (!room_name || strlen(room_name) == 0) {
        safe_send(cli->sock, "[Server] Usage: /create <room_name>\n", TYPE_ERROR);
        return;
    }
    // Check max room name length using struct member size
    if (strlen(room_name) >= sizeof(((Room*)0)->name)) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room name too long (max %zu characters).\n", sizeof(((Room*)0)->name) - 1);
        safe_send(cli->sock, error_msg, TYPE_ERROR);
        return;
    }
    if (cli->room != NULL) {
        safe_send(cli->sock, "[Server] You are already in a room. Please /leave first.\n", TYPE_ERROR);
        return;
    }

    // Check for existing room name and assign ID under lock
    pthread_mutex_lock(&g_rooms_mutex);
    Room *existing_room_check = find_room_unlocked(room_name); // Check name before assigning ID
    if (existing_room_check != NULL) {
        pthread_mutex_unlock(&g_rooms_mutex);
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room name '%s' already exists.\n", room_name);
        safe_send(cli->sock, error_msg, TYPE_ERROR);
        return;
    }
    // ID assignment and adding to list are done under this lock
    unsigned int new_id = g_next_room_id++;

    Room *new_room = (Room *)malloc(sizeof(Room));
    if (!new_room) {
        perror("malloc for new room failed");
        safe_send(cli->sock, "[Server] Failed to create room.\n", TYPE_ERROR);
        return;
    }
    strncpy(new_room->name, room_name, sizeof(new_room->name) - 1);
    new_room->name[sizeof(new_room->name) - 1] = '\0';
    new_room->id = new_id; // Assign the new ID
    new_room->members = NULL;
    new_room->member_count = 0;
    new_room->next = NULL;
    new_room->prev = NULL;
	new_room->owner = cli; // 방장을 지정

    list_add_room_unlocked(new_room); // Add to list while still holding lock
    pthread_mutex_unlock(&g_rooms_mutex);

    // Add member (this will re-lock g_rooms_mutex internally, which is fine)
    room_add_member(new_room, cli);

    printf("[INFO] Client %s created room '%s' (ID: %u) and joined.\n", cli->nick, new_room->name, new_room->id);
    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] Room '%s' (ID: %u) created and joined.\n", new_room->name, new_room->id);
    safe_send(cli->sock, new_room->name, TYPE_CREATE);
}

void cmd_join_room(Client *cli, const char *name) {
    Room* target_room = find_room(name);
    if (!target_room) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room with name %s not found.\n", name);
        safe_send(cli->sock, error_msg, TYPE_ERROR);
        return;
    }
    room_add_member(target_room, cli);
    printf("[INFO] Client %s joined room '%s' (ID: %u).\n", cli->nick, target_room->name, target_room->id);
    
    safe_send(cli->sock, name, TYPE_JOIN);
    broadcast_room(target_room, cli, "[Server] %s joined the room.\n", cli->nick);
}

void cmd_leave_room(Client *cli) {
    if (cli->room == NULL) {
        safe_send(cli->sock, "[Server] You are not in any room.\n", TYPE_ERROR);
        return;
    }
    Room *current_room = cli->room;
    broadcast_room(current_room, cli, "[Server] %s left the room.\n", cli->nick);
    room_remove_member(current_room, cli); // This also sets cli->room = NULL
    printf("[INFO] Client %s left room '%s'.\n", cli->nick, current_room->name);
    safe_send(cli->sock, "[Server] You left the room.\n", TYPE_LEAVE);
    destroy_room_if_empty(current_room);
}

void cmd_friends(Client* client) {
	pthread_mutex_lock(&g_clients_mutex);
    char buffer[BUFFER_SIZE] = { 0 }; // 충분히 큰 버퍼를 준비
    strcpy(buffer, "Friends:\n");

    for (int i = 0; i < client->friend_count; ++i) {
        if (client->friend_list[i] != NULL) {
            strcat(buffer, client->friend_list[i]->nick);
            strcat(buffer, "\n");
        }
    }
	pthread_mutex_unlock(&g_clients_mutex);

    safe_send(client->sock, buffer, TYPE_FRIENDS);
}

void cmd_add_friend(Client *cli, const char *friend_nick) {
    if (!friend_nick || strlen(friend_nick) == 0) {
        safe_send(cli->sock, "[Server] Usage: /addfriend <nickname>\n", TYPE_ERROR);
        return;
    }
    Client *friend_cli = find_client_by_nick(friend_nick);
    char msg[BUFFER_SIZE];
    // 해당 이름의 친구가 없는 경우
    if (!friend_cli) {
        snprintf(msg, sizeof(msg), "Failed %s", friend_nick);
        safe_send(cli->sock, msg, TYPE_ADD_FRIEND);
        return;
    }
    if (friend_cli == cli) {
        safe_send(cli->sock, "[Server] You cannot add yourself as a friend.\n", TYPE_ERROR);
        return;
    }
    // 해당 이름의 사용자가 있는 경우
	list_add_friend(cli, friend_cli); // Add friend under lock
    snprintf(msg, sizeof(msg), "Succeed %s", friend_cli->nick);
    safe_send(cli->sock, msg, TYPE_ADD_FRIEND);
}

void cmd_remove_friend(Client *cli, const char *friend_nick) {
    if (!friend_nick || strlen(friend_nick) == 0) {
        safe_send(cli->sock, "[Server] Usage: /removefriend <nickname>\n", TYPE_ERROR);
        return;
    }
    Client *friend_cli = find_client_by_nick(friend_nick);
    if (!friend_cli) {
        safe_send(cli->sock, "[Server] User not found.\n", TYPE_ERROR);
        return;
    }
    if (friend_cli == cli) {
        safe_send(cli->sock, "[Server] You cannot remove yourself as a friend.\n", TYPE_ERROR);
        return;
    }
    list_remove_friend(cli, friend_cli); // Remove friend under lock
    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] Removed %s from your friends list.\n", friend_cli->nick);
    safe_send(cli->sock, success_msg, TYPE_REMOVE_FRIEND);
}

void get_nickname(Client *user) {
    // Assign a random nickname, e.g., "User12345"
    char temp_nick_buffer[sizeof(user->nick)];
    int attempt = 0;
    const int MAX_NICK_GENERATION_ATTEMPTS = 10;

    pthread_mutex_lock(&g_clients_mutex);
    while (attempt < MAX_NICK_GENERATION_ATTEMPTS) {
        snprintf(temp_nick_buffer, sizeof(temp_nick_buffer), "User%d", rand() % 100000);
        // The current 'user' is in g_clients but its 'nick' field is not yet 'temp_nick_buffer'.
        // find_client_by_nick_unlocked checks against other clients' established nicks.
        Client *existing_client = find_client_by_nick_unlocked(temp_nick_buffer);
        
        if (existing_client == NULL) { // Unique name found
            strncpy(user->nick, temp_nick_buffer, sizeof(user->nick) - 1);
            user->nick[sizeof(user->nick) - 1] = '\0';
            break; 
        }
        attempt++;
    }
    pthread_mutex_unlock(&g_clients_mutex);

    if (attempt == MAX_NICK_GENERATION_ATTEMPTS) {
        // Fallback if unique random name generation failed
        long fallback_suffix = (long)time(NULL) ^ (long)(intptr_t)user;
        snprintf(user->nick, sizeof(user->nick), "Guest%ld", fallback_suffix % 100000);
        user->nick[sizeof(user->nick) - 1] = '\0';
    }

    printf("[INFO] New user connected: %s (fd %d)\n", user->nick, user->sock);
}

void print_buffer_hex(const unsigned char* buf, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        printf("%02X ", buf[i]);
    }
    printf("\n");
}

void *client_process(void *arg) {
    Client *cli = (Client *)arg;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    get_nickname(cli);
    // If get_nickname failed, cli->sock might be bad or cli->nick is "Guest".
    // The client struct is already added to the global list in main.

    while (cli->sock >= 0) {
        bytes_received = recv(cli->sock, buffer, sizeof(buffer) - 1, 0);

        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                printf("[INFO] Client %s (fd %d) disconnected.\n", cli->nick, cli->sock);
            } else {
                // EAGAIN/EWOULDBLOCK might occur if RCVTIMEO was set and expired.
                // Otherwise, it's a more serious error.
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("recv error");
                    printf("[ERROR] Recv error on client %s (fd %d).\n", cli->nick, cli->sock);
                }
            }
            break;
        }
        buffer[bytes_received] = '\0';
        buffer[strcspn(buffer, "\n")] = 0; // Remove trailing newline

        // 클라이언트에서 패킷을 전송할 때 메시지 구조
        // [TYPE_TEXT][length][message]
        // send로 패킷헤더 구조체를 전송한다면, 문자열로 전송이 되나? 아니면 송신자 측에서 전송 타입을 결정할 수 있나?
        // --> 이진 데이터 형태로 전송이 됨.
        if (bytes_received < sizeof(PacketHeader)) {
            safe_send(cli->sock, "[Server] Invalid packet received.\n", TYPE_ERROR);
			printf("[Debug] Received packet too small: %zd bytes\n", bytes_received);
            continue;
        }
        // 패킷 헤더를 읽어오기
        PacketHeader header;
        memcpy(&header, buffer, sizeof(PacketHeader));
        header.length = ntohs(header.length); // Convert length to host byte order
		printf("[DEBUG] Received packet type: %d, length: %d\n", header.type, header.length);

        print_buffer_hex(buffer, sizeof(PacketHeader));

		char* message_start = buffer + sizeof(PacketHeader);
        if (header.length > 0)
		{
			// Ensure the message is null-terminated
			message_start[header.length] = '\0';
		}
        else {
            // Invalid length, skip processing
            safe_send(cli->sock, "[Server] Invalid message length.\n", TYPE_ERROR);
            continue;
        }

		// "/" 대신 header.type를 사용하여 명령어를 처리
        switch (header.type)
        {
        case TYPE_TEXT:
            // Handle text message
            printf("[DEBUG] Received message from %s: %s\n", cli->nick, message_start);
            if (cli->room != NULL) {
                broadcast_room(cli->room, cli, "[%s] %s\n", cli->nick, message_start);
            }
            else {
                safe_send(cli->sock, "[Server] You must join a room to send messages. Type /rooms or /create.\n", TYPE_ERROR);
            }
            break;
        case TYPE_NICK:
            Client* existing = find_client_by_nick_unlocked(message_start); // 이름 중복 검사
            if (existing != NULL && existing != cli) {
                char error_msg[BUFFER_SIZE];
                snprintf(error_msg, sizeof(error_msg), "[Server] Nickname '%s' is already taken.\n", message_start);
                safe_send(cli->sock, error_msg, TYPE_ERROR);
            }
            else {
                printf("[INFO] %s changed nickname to %s\n", cli->nick, message_start);
                strncpy(cli->nick, message_start, sizeof(cli->nick) - 1);
                cli->nick[sizeof(cli->nick) - 1] = '\0';
                //char success_msg[BUFFER_SIZE];
                //snprintf(success_msg, sizeof(success_msg), "[Server] Nickname updated to %s.\n", cli->nick);
                //safe_send(cli->sock, success_msg);
            }
            break;
		case TYPE_ROOMS:
            cmd_rooms(cli->sock);
            break;
        case TYPE_CREATE:
        {
			if (header.length <= 0 || header.length >= sizeof(cli->nick)) {
				safe_send(cli->sock, "[Server] Invalid room name length.\n", TYPE_ERROR);
				break;
			}
            char* room_name = strtok(message_start, "\n");
            cmd_create_room(cli, room_name);
            break;
        }
        case TYPE_JOIN:
        {
            char* room_name = strtok(message_start, "\n");
            cmd_join_room(cli, room_name);
            break;
        }
		case TYPE_LEAVE:
            cmd_leave_room(cli);
            break;
		case TYPE_USERS:
            cmd_users(cli);
            break;
        case TYPE_FRIENDS:
			cmd_friends(cli);
            break;
        case TYPE_CHANGE:
        {
            char* room_name = strtok(NULL, " "); // this is room name
            cmd_change(cli, room_name);
            break;
        }
        case TYPE_KICK :
            char* target_nick = strtok(message_start, "\n"); // this is target nick
            cmd_kick(cli, target_nick);
            break;
        case TYPE_ADD_FRIEND:
            cmd_add_friend(cli, message_start);
            break;
        case TYPE_REMOVE_FRIEND:
        {
            // Handle removing a friend
            if (header.length > 0 && header.length < sizeof(cli->nick)) {
                char* friend_nick = buffer + sizeof(PacketHeader);
                friend_nick[header.length] = '\0'; // Null-terminate the friend's nickname
                cmd_remove_friend(cli, friend_nick);
            }
            else {
                safe_send(cli->sock, "[Server] Invalid friend nickname length.\n", TYPE_ERROR);
            }
            break;
        }
        default:
            break;
        }
    }

    printf("[INFO] Cleaning up client %s (fd %d).\n", cli->nick, cli->sock);
    if (cli->room) {
        Room *r = cli->room;
        room_remove_member(r, cli);
        // Notify remaining members *after* removing the client
        // but before potentially destroying the room.
        broadcast_room(r, NULL, "[Server] %s disconnected.\n", cli->nick);
        destroy_room_if_empty(r);
    }
    list_remove_client(cli);
    if (cli->sock >= 0) {
        close(cli->sock);
        cli->sock = -1;
    }
    free(cli);
    printf("[INFO] Client structure freed.\n");
    pthread_exit(NULL);
}

void process_server_command(int epfd, int server_sock) {
    char command_buf[64];
    if (fgets(command_buf, sizeof(command_buf) - 1, stdin) == NULL) {
        printf("\n[INFO] Server shutting down (EOF on stdin).\n");
        // Graceful shutdown sequence
        close(server_sock); // Stop accepting new connections
        pthread_mutex_lock(&g_clients_mutex);
        Client *current_cli = g_clients;
        while(current_cli != NULL) {
            if (current_cli->sock >= 0) {
                printf("[INFO] Closing socket for client %s (fd %d) for shutdown.\n", current_cli->nick, current_cli->sock);
                shutdown(current_cli->sock, SHUT_RDWR);
                close(current_cli->sock);
                current_cli->sock = -1;
            }
            current_cli = current_cli->next;
        }
        pthread_mutex_unlock(&g_clients_mutex);

        pthread_mutex_lock(&g_rooms_mutex);
        Room *current_room = g_rooms;
        while(current_room != NULL) {
            Room *next_room = current_room->next;
            printf("[INFO] Freeing room '%s' during shutdown.\n", current_room->name);
            free(current_room);
            current_room = next_room;
        }
        g_rooms = NULL;
        pthread_mutex_unlock(&g_rooms_mutex);
        close(epfd);
        exit(EXIT_SUCCESS);
    }
    command_buf[strcspn(command_buf, "\n")] = 0;
    char *cmd = strtok(command_buf, " ");

    if (!cmd || strlen(cmd) == 0) { /* No action for empty command */ }
    else if (strcmp(cmd, "users") == 0) {
        printf("--- Connected Users ---\n");
        pthread_mutex_lock(&g_clients_mutex);
        Client *current = g_clients;
        if (current == NULL) printf("No users connected.\n");
        else {
            while (current != NULL) {
                if (current->room) {
                    printf("  - %s (fd %d, Room: '%s' ID: %u)\n",
                           current->nick, current->sock, current->room->name, current->room->id);
                } else {
                    printf("  - %s (fd %d, Room: None)\n",
                           current->nick, current->sock);
                }
                current = current->next;
            }
        }
        pthread_mutex_unlock(&g_clients_mutex);
        printf("-----------------------\n");
    } else if (strcmp(cmd, "rooms") == 0) {
        printf("--- Chat Rooms ---\n");
        pthread_mutex_lock(&g_rooms_mutex);
        Room *current_room = g_rooms;
        if (current_room == NULL) printf("No rooms created.\n");
        else {
            while (current_room != NULL) {
                printf("  - ID %u: Room '%s' (%d members):\n", current_room->id, current_room->name, current_room->member_count);
                Client *member = current_room->members;
                while (member != NULL) {
                    printf("    - %s (fd %d)\n", member->nick, member->sock);
                    member = member->room_next;
                }
                current_room = current_room->next;
            }
        }
        pthread_mutex_unlock(&g_rooms_mutex);
        printf("------------------\n");
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
         printf("[INFO] Server shutting down (command).\n");
         // Graceful shutdown (same logic as EOF)
         close(server_sock);
         pthread_mutex_lock(&g_clients_mutex);
         Client *current = g_clients;
         while(current != NULL) {
             if (current->sock >= 0) {
                 printf("[INFO] Closing socket for client %s (fd %d) for shutdown.\n", current->nick, current->sock);
                 shutdown(current->sock, SHUT_RDWR); close(current->sock); current->sock = -1;
             }
             current = current->next;
         }
         pthread_mutex_unlock(&g_clients_mutex);
         pthread_mutex_lock(&g_rooms_mutex);
         Room *current_room = g_rooms;
         while(current_room != NULL) {
             Room *next_room = current_room->next;
             printf("[INFO] Freeing room '%s' during shutdown.\n", current_room->name);
             free(current_room); current_room = next_room;
         }
         g_rooms = NULL; pthread_mutex_unlock(&g_rooms_mutex);
         close(epfd);
         exit(EXIT_SUCCESS);
    } else {
        printf("[Server] Unknown server command: %s. Available: users, rooms, quit\n", cmd);
    }
    printf("> "); fflush(stdout);
}

int main() {
    int server_sock;
    struct sockaddr_in sin, cli_addr;
    socklen_t clientlen = sizeof(cli_addr);

    // Seed random number generator
    srand(time(NULL));

    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket creation failed"); exit(EXIT_FAILURE);
    }
    int optval = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("setsockopt SO_REUSEADDR failed"); close(server_sock); exit(EXIT_FAILURE);
    }
    memset((char *)&sin, '\0', sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(PORTNUM);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(server_sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind failed"); close(server_sock); exit(EXIT_FAILURE);
    }
    if (listen(server_sock, 10) < 0) {
        perror("listen failed"); close(server_sock); exit(EXIT_FAILURE);
    }
    printf("Server listening on port %d\n", PORTNUM);

    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1 failed"); close(server_sock); exit(EXIT_FAILURE);
    }
    struct epoll_event ev, events[MAX_EVENTS];

    ev.events = EPOLLIN; ev.data.fd = server_sock;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_sock, &ev) == -1) {
        perror("epoll_ctl: add server_sock failed"); close(epfd); close(server_sock); exit(EXIT_FAILURE);
    }
    ev.events = EPOLLIN; ev.data.fd = STDIN_FILENO;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, STDIN_FILENO, &ev) == -1) {
        perror("epoll_ctl: add STDIN_FILENO failed"); close(epfd); close(server_sock); exit(EXIT_FAILURE);
    }
    printf("> "); fflush(stdout);

    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait failed"); break;
        }
        for (int i = 0; i < nfds; i++) {
            int current_fd = events[i].data.fd;
            if (current_fd == server_sock) {
                int client_sock = accept(server_sock, (struct sockaddr *)&cli_addr, &clientlen);
                if (client_sock < 0) {
                    // Handle accept errors, especially for non-blocking accept (though this is blocking)
                    perror("accept failed"); continue;
                }
                Client *new_client = (Client *)malloc(sizeof(Client));
                if (!new_client) {
                    perror("malloc for new client failed"); close(client_sock);
                    fprintf(stderr, "[ERROR] Failed to allocate memory for new client.\n"); continue;
                }
                memset(new_client, 0, sizeof(Client));
                new_client->sock = client_sock;

                // Add client to global list *before* thread creation for visibility.
                list_add_client(new_client);

                if (pthread_create(&(new_client->thread), NULL, client_process, new_client) != 0) {
                    perror("pthread_create failed");
                    list_remove_client(new_client); // Clean up if thread creation fails
                    close(new_client->sock); free(new_client);
                    fprintf(stderr, "[ERROR] Failed to create thread for new client.\n"); continue;
                }
                pthread_detach(new_client->thread); // Resources freed automatically on thread exit
            } else if (current_fd == STDIN_FILENO) {
                process_server_command(epfd, server_sock);
            }
            // Client sockets are handled by their respective threads.
            // Epoll in main only handles server_sock and stdin.
        }
    }
    // Cleanup if loop breaks unexpectedly (normally handled by 'quit' or EOF)
    close(epfd);
    close(server_sock);
    return 0;
}
