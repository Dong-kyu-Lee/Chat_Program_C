#include <gtk/gtk.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h> // For bool type

#define BUFFER_SIZE 1024

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

// 친구 목록 예시 데이터
const char* friends[] = { "Lilly", "Marco", "Father" };
const int num_friends = 3;

// 채팅방 목록 예시 데이터
const char* rooms[] = { "Room Name", "Room Name 1", "Room Name 2" };
const int members[] = { 4, 2, 5 };
const int num_rooms = 3;

// 전역 위젯 포인터 (탭 전환용)
GtkWidget* stack;
GtkWidget* friend_tab_btn;
GtkWidget* chat_tab_btn;
GtkWidget* chat_tab;
GtkWidget* friend_tab;

// UI elements
GtkWidget *ip_entry;
GtkWidget *port_entry;
GtkWidget *name_entry; // Declare label_name globally
GtkWidget *connect_button;
GtkWidget *chat_view;
GtkWidget *message_entry;
GtkWidget *send_button;
GtkWidget *window; // Enter window
GtkWidget* main_window;
GtkWidget *scrolled_window; // Declare scrolled_window globally

int sock = -1; // Socket descriptor for the server connection
pthread_t recv_thread_id = 0; // Thread ID for the receiver thread

// Structure to pass widget and state to idle callback
typedef struct {
    GtkWidget *widget;
    gboolean sensitive;
} WidgetSensitivityData;

// Idle callback to safely append message to chat view from another thread
// Returns G_SOURCE_REMOVE to be called only once
// user_data is a duplicated string (g_strdup)
static gboolean append_message_to_view_idle(gpointer user_data) {
    char *message = (char *)user_data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(chat_view));
    GtkTextIter end_iter;
    gtk_text_buffer_get_end_iter(buffer, &end_iter);
    gtk_text_buffer_insert(buffer, &end_iter, message, -1);
    gtk_text_buffer_insert(buffer, &end_iter, "\n", -1);

    // Auto-scroll to the end of the text view
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled_window));
    if (vadj) {
        gtk_adjustment_set_value(vadj, gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj));
    }
    g_free(message); // Free the string allocated by g_strdup
    return G_SOURCE_REMOVE; // Remove source after execution
}

// Idle callback to safely set widget sensitivity from another thread
// Returns G_SOURCE_REMOVE to be called only once
// user_data is a pointer to WidgetSensitivityData (g_new)
static gboolean set_widget_sensitive_idle(gpointer user_data) {
    WidgetSensitivityData *data = (WidgetSensitivityData *)user_data;
    gtk_widget_set_sensitive(data->widget, data->sensitive);
    g_free(data); // Free the allocated data structure
    return G_SOURCE_REMOVE;
}

// Thread function to receive messages from the server
static void *receive_messages(void *arg) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    while ((bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        g_idle_add(append_message_to_view_idle, g_strdup(buffer)); // Use g_idle_add for thread safety
    }

    if (bytes_received == 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Disconnected from server."));
    } else if (bytes_received < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error receiving message."));
    }

    // Safely update UI elements (disable sending, enable connect)
    WidgetSensitivityData *send_data = g_new(WidgetSensitivityData, 1);
    send_data->widget = send_button; send_data->sensitive = FALSE;
    g_idle_add(set_widget_sensitive_idle, send_data);
    WidgetSensitivityData *msg_data = g_new(WidgetSensitivityData, 1);
    msg_data->widget = message_entry; msg_data->sensitive = FALSE;
    g_idle_add(set_widget_sensitive_idle, msg_data);
    WidgetSensitivityData *conn_data = g_new(WidgetSensitivityData, 1);
    conn_data->widget = connect_button; conn_data->sensitive = TRUE;
    g_idle_add(set_widget_sensitive_idle, conn_data);

    if (sock != -1) {
        close(sock);
        sock = -1;
    }
    recv_thread_id = 0; // Mark thread as finished
    return NULL;
}

// 이 함수는 main_window가 닫힐 때 main_window를 NULL로 설정하는 콜백입니다.
static void on_main_window_destroy(GtkWidget* widget, gpointer data) {
    main_window = NULL;
}

// 친구탭 UI 생성
GtkWidget* create_friends_tab() {
    friend_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    for (int i = 0; i < num_friends; i++) {
		printf("Creating friend tab for %s\n", friends[i]);
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        GtkWidget* icon = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_DIALOG);
        GtkWidget* label = gtk_label_new(friends[i]);
        gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(friend_tab), row, FALSE, FALSE, 0);
    }
    gtk_widget_set_halign(friend_tab, GTK_ALIGN_START);
    return friend_tab;
}

// 채팅탭 UI 생성
GtkWidget* create_chats_tab() {
    chat_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    for (int i = 0; i < num_rooms; i++) {
		printf("Creating chat tab for %s with %d members\n", rooms[i], members[i]);
        GtkWidget* frame = gtk_frame_new(NULL);
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        GtkWidget* label = gtk_label_new(rooms[i]);
        char member_str[32];
        snprintf(member_str, sizeof(member_str), "Members: %d", members[i]);
        GtkWidget* member_label = gtk_label_new(member_str);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(row), member_label, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(frame), row);
        gtk_box_pack_start(GTK_BOX(chat_tab), frame, FALSE, FALSE, 0);
    }
    gtk_widget_set_halign(chat_tab, GTK_ALIGN_START);
    return chat_tab;
}

// 탭 전환 콜백
void switch_tab(GtkButton* button, gpointer user_data) {
    if (!stack || !friend_tab_btn || !chat_tab_btn) return;
    const char* tab = (const char*)user_data;
    // 신호 차단
    g_signal_handlers_block_by_func(friend_tab_btn, switch_tab, "friends");
    g_signal_handlers_block_by_func(chat_tab_btn, switch_tab, "chats");

    if (friend_tab) {
        gtk_container_remove(GTK_CONTAINER(stack), friend_tab);
        friend_tab = NULL; // 참조 해제
    }
    if (chat_tab) {
        gtk_container_remove(GTK_CONTAINER(stack), chat_tab);
        chat_tab = NULL; // 참조 해제
    }

    // 탭 갱신
	if (g_strcmp0(tab, "friends") == 0 && !friend_tab) {
		friend_tab = create_friends_tab();
		printf("Recreate friends tab\n");
		gtk_stack_add_named(GTK_STACK(stack), friend_tab, "friends");
        gtk_widget_show_all(friend_tab);
	}
	else if (g_strcmp0(tab, "chats") == 0 && !chat_tab) {
		chat_tab = create_chats_tab();
		printf("Recreate chat tab\n");
		gtk_stack_add_named(GTK_STACK(stack), chat_tab, "chats");
		gtk_widget_show_all(chat_tab);
	}

    // 탭 전환
    gtk_stack_set_visible_child_name(GTK_STACK(stack), tab);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(friend_tab_btn), g_strcmp0(tab, "friends") == 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chat_tab_btn), g_strcmp0(tab, "chats") == 0);
    printf("Switch\n");

    // 신호 재연결
    g_signal_handlers_unblock_by_func(friend_tab_btn, switch_tab, "friends");
    g_signal_handlers_unblock_by_func(chat_tab_btn, switch_tab, "chats");
}

// 하단 사용자 정보/버튼 UI 생성
GtkWidget* create_bottom_bar() {
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* user_icon = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_DIALOG);
    GtkWidget* user_label = gtk_label_new("John");
    GtkWidget* create_btn = gtk_link_button_new_with_label("Create Room", ""); // 실제 동작은 콜백 연결 필요
    GtkWidget* find_btn = gtk_link_button_new_with_label("Find Friend", "");
    gtk_box_pack_start(GTK_BOX(bar), user_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), user_label, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bar), find_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bar), create_btn, FALSE, FALSE, 0);
    return bar;
}

// Callback for the "Enter" button
static void on_enter_button_clicked(GtkWidget *widget, gpointer data) {
    if (main_window == NULL) {
        GtkWidget* main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(main_window), "Chat App");
        gtk_window_set_default_size(GTK_WINDOW(main_window), 350, 500);

        GtkWidget* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        // 상단 탭 버튼
        GtkWidget* tab_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        friend_tab_btn = gtk_toggle_button_new();
        GtkWidget* friend_icon = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_BUTTON);
        gtk_container_add(GTK_CONTAINER(friend_tab_btn), friend_icon);
        chat_tab_btn = gtk_toggle_button_new();
        GtkWidget* chat_icon = gtk_image_new_from_icon_name("mail-message-new", GTK_ICON_SIZE_BUTTON);
        gtk_container_add(GTK_CONTAINER(chat_tab_btn), chat_icon);

        gtk_box_pack_start(GTK_BOX(tab_bar), friend_tab_btn, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(tab_bar), chat_tab_btn, TRUE, TRUE, 0);

        // 스택(탭 내용)
        stack = gtk_stack_new();
        gtk_stack_add_named(GTK_STACK(stack), create_friends_tab(), "friends");
        gtk_stack_add_named(GTK_STACK(stack), create_chats_tab(), "chats");
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "friends");

        // 하단 바
        GtkWidget* bottom_bar = create_bottom_bar();

        gtk_box_pack_start(GTK_BOX(main_box), tab_bar, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(main_box), stack, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(main_box), bottom_bar, FALSE, FALSE, 0);

        gtk_container_add(GTK_CONTAINER(main_window), main_box);

        // 탭 버튼 콜백 연결
        g_signal_connect(friend_tab_btn, "clicked", G_CALLBACK(switch_tab), "friends");
        g_signal_connect(chat_tab_btn, "clicked", G_CALLBACK(switch_tab), "chats");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(friend_tab_btn), TRUE);
        /*main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(main_window), "Main Window");
        gtk_window_set_default_size(GTK_WINDOW(main_window), 400, 300);
        g_signal_connect(main_window, "destroy", G_CALLBACK(on_main_window_destroy), NULL);
        gtk_widget_set_sensitive(connect_button, FALSE);*/
        gtk_widget_show_all(main_window);
    }
    else {
        // 이미 열려 있다면 포커스만 줍니다 (선택적)
        gtk_window_present(GTK_WINDOW(main_window));
    }
    /*if (sock != -1) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Already connected or attempting to connect."));
        return;
    }
    const char *ip_address = gtk_entry_get_text(GTK_ENTRY(ip_entry));
    const char *port_str = gtk_entry_get_text(GTK_ENTRY(port_entry));
    int port = atoi(port_str);

    if (strlen(ip_address) == 0 || port <= 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Please enter a valid IP address and port number."));
        return;
    }

    struct sockaddr_in server_addr;
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Socket creation failed."));
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip_address, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Invalid IP address format."));
        close(sock); sock = -1; return;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Connection failed."));
        close(sock); sock = -1; return; // Close socket on connection failure
    }

    g_idle_add(append_message_to_view_idle, g_strdup("[Client] Connected to server!"));
    gtk_widget_set_sensitive(connect_button, FALSE);
    gtk_widget_set_sensitive(send_button, TRUE);
    gtk_widget_set_sensitive(message_entry, TRUE);
    gtk_widget_grab_focus(message_entry);

    if (pthread_create(&recv_thread_id, NULL, receive_messages, NULL) != 0) {
        perror("pthread_create for receive_messages");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Failed to create receive thread."));
        close(sock); sock = -1;
        gtk_widget_set_sensitive(connect_button, TRUE);
        gtk_widget_set_sensitive(send_button, FALSE);
        gtk_widget_set_sensitive(message_entry, FALSE);
    }*/
}

// Callback for the "Send" button or pressing Enter in the message entry
static void on_send_clicked(GtkWidget *widget, gpointer data) {
    if (sock < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Not connected to server."));
        return;
    }
    const char *message = gtk_entry_get_text(GTK_ENTRY(message_entry));
    if (strlen(message) == 0) return;

    char buffer_to_send[BUFFER_SIZE];
    PacketHeader header;
    header.type = TYPE_TEXT;
    header.length = htons(strlen(message) + 1); // 개행 문자 포함 길이

    memcpy(buffer_to_send, &header, sizeof(PacketHeader));
    snprintf(buffer_to_send + sizeof(PacketHeader), BUFFER_SIZE - sizeof(PacketHeader), "%s\n", message);
    // 패킷 길이 디버그
    int packet_len = sizeof(PacketHeader) + strlen(message) + 1; // header + 메시지 + 개행
    printf("[DEBUG] Packet length: %d\n", packet_len);
	
    // Send the message to the server
    if (send(sock, buffer_to_send, packet_len, 0) < 0) {
        perror("send");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Failed to send message."));
    }
    gtk_entry_set_text(GTK_ENTRY(message_entry), "");
    gtk_widget_grab_focus(message_entry);
}

// Window destroy callback (cleanup)
static void on_window_destroy(GtkWidget *widget, gpointer data) {
    if (sock != -1) {
        shutdown(sock, SHUT_RDWR); close(sock); sock = -1;
    }
    if (recv_thread_id != 0) { // If thread was created and potentially running
        pthread_join(recv_thread_id, NULL); // Wait for the receive thread to finish
    }
    gtk_main_quit(); // Quit GTK main loop
}

// Application activate callback (builds UI)
static void activate(GtkApplication *app, gpointer user_data) {
    GtkWidget *grid, *ip_label, *port_label, *name_label; // Ensure scrolled_window is NOT declared locally here

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Chat Client");
    gtk_window_set_default_size(GTK_WINDOW(window), 350, 180);
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL); // Connect cleanup on window close

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_container_add(GTK_CONTAINER(window), grid);

    ip_label = gtk_label_new("Server IP:");
    gtk_grid_attach(GTK_GRID(grid), ip_label, 0, 0, 1, 1); // Row 0, Col 0
    ip_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ip_entry), "127.0.0.1");
    gtk_grid_attach(GTK_GRID(grid), ip_entry, 1, 0, 2, 1); 

    port_label = gtk_label_new("Port:");
    gtk_grid_attach(GTK_GRID(grid), port_label, 0, 1, 1, 1); 
    port_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(port_entry), "9000");
    gtk_grid_attach(GTK_GRID(grid), port_entry, 1, 1, 2, 1); 

    name_label = gtk_label_new("Name:");
    name_entry = gtk_entry_new();
    gtk_grid_attach(GTK_GRID(grid), name_label, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), name_entry, 1, 2, 2, 1);

    connect_button = gtk_button_new_with_label("Enter");
    g_signal_connect(connect_button, "clicked", G_CALLBACK(on_enter_button_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), connect_button, 2, 3, 1, 1);

    /*scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC); // Add scrollbars
    gtk_widget_set_vexpand(scrolled_window, TRUE);
    chat_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(chat_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(chat_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(chat_view), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scrolled_window), chat_view);
    gtk_grid_attach(GTK_GRID(grid), scrolled_window, 0, 1, 5, 1); // Row 1, Col 0, spans 5 columns

    message_entry = gtk_entry_new();
    gtk_widget_set_hexpand(message_entry, TRUE);
    g_signal_connect(message_entry, "activate", G_CALLBACK(on_send_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), message_entry, 0, 2, 4, 1); // Row 2, Col 0, spans 4 columns
    gtk_widget_set_sensitive(message_entry, FALSE);

    send_button = gtk_button_new_with_label("Send");
    g_signal_connect(send_button, "clicked", G_CALLBACK(on_send_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), send_button, 4, 2, 1, 1); // Row 2, Col 4
    gtk_widget_set_sensitive(send_button, FALSE);*/

    gtk_widget_show_all(window);
}

int main(int argc, char *argv[]) {
    GtkApplication *app;
    int status;

    app = gtk_application_new("org.example.chatclient", G_APPLICATION_NON_UNIQUE); // Use recommended flag
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return status;
}
