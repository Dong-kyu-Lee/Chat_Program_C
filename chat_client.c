#include <gtk/gtk.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h> // For bool type
#include "header.h"

#define BUFFER_SIZE 1024

// 친구 목록 예시 데이터
char* friends[20];
int num_friends = 0;

// 채팅방 목록 예시 데이터
char* rooms[20];
int members[20];
int num_rooms = 0;

// 방에 존재하는 유저 목록 예시 데이터
char* users_in_room[20];
int num_users_in_room = 0;

// 특정 방에 존재하는 투표 목록 예시 데이터
Vote* votes[20];
int num_votes = 0;

// 전역 위젯 포인터 (탭 전환용)
GtkWidget* stack;
GtkWidget* friend_tab_btn;
GtkWidget* chat_tab_btn;
GtkWidget* chat_tab;
GtkWidget* friend_tab;

GtkWidget* find_friend_dialog;
GtkWidget* user_list; // 방에 존재하는 유저 목록


// UI elements
GtkWidget *ip_entry;
GtkWidget *port_entry;
GtkWidget *name_entry; // Declare label_name globally
GtkWidget *connect_button;
GtkWidget *chat_view;
GtkWidget* entry; // 채팅 입력창
GtkWidget *send_button;
GtkWidget *window; // Enter window
GtkWidget* main_window;
GtkWidget *scrolled_window; // Declare scrolled_window globally
GtkWidget* listbox;

int sock = -1; // Socket descriptor for the server connection
pthread_t recv_thread_id = 0; // Thread ID for the receiver thread

// Structure to pass widget and state to idle callback
typedef struct {
    GtkWidget *widget;
    gboolean sensitive;
} WidgetSensitivityData;

void send_message(PacketType type, const char* message) {
	if (sock < 0) {
		printf("[DEBUG] Not connected to server, cannot send message: %s\n", message);
		return;
	}
	char buffer[BUFFER_SIZE];
	PacketHeader header;
	header.type = type;
	header.length = htons(strlen(message) + 1); // Include null terminator
	memcpy(buffer, &header, sizeof(PacketHeader));
	snprintf(buffer + sizeof(PacketHeader), BUFFER_SIZE - sizeof(PacketHeader), "%s\n", message);
	printf("[DEBUG] Sending packet type: %d, length: %d, message: %s\n", type, ntohs(header.length), message);

	// Send the message to the server
	if (send(sock, buffer, sizeof(PacketHeader) + strlen(message) + 1, 0) < 0) {
		perror("send");
		printf("[DEBUG] Failed to send message: %s\n", message);
	}
}

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

// 이 함수는 main_window가 닫힐 때 main_window를 NULL로 설정하는 콜백입니다.
static void on_main_window_destroy(GtkWidget* widget, gpointer data) {
    main_window = NULL;
}

// "친구 추가 완료" 다이얼로그
gboolean show_added_friend_dialog_idle(gpointer data) {
	const char* name = (const char*)data;
    char msg[128];
    snprintf(msg, sizeof(msg), "You added friend named \"%s\"", name);
    GtkWidget* dialog = gtk_message_dialog_new(
        NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_CLOSE,
        "%s", msg
    );
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

// "유저 없음" 다이얼로그
gboolean show_no_user_dialog_idle(gpointer data) {
	const char* name = (const char*)data;
    char msg[128];
    snprintf(msg, sizeof(msg), "There's no user named \"%s\"", name);
    GtkWidget* dialog = gtk_message_dialog_new(
        NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_CLOSE,
        "%s", msg
    );
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

// Find 버튼 콜백
void on_find_clicked(GtkButton* button, gpointer user_data) {
    GtkEntry* entry = GTK_ENTRY(user_data);
    const char* name = gtk_entry_get_text(entry);
    GtkWindow* parent = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(entry)));

    // 서버에 친구 존재 여부 확인
	send_message(TYPE_ADD_FRIEND, name);
}

// Find Friend 다이얼로그
void on_find_friend(GtkButton* button, gpointer user_data) {
    find_friend_dialog = gtk_dialog_new_with_buttons(
        "Find friends by name",
        GTK_WINDOW(user_data),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_OK", GTK_RESPONSE_OK,
        NULL
    );
    GtkWidget* content_area = gtk_dialog_get_content_area(GTK_DIALOG(find_friend_dialog));
    GtkWidget* entry = gtk_entry_new();
    GtkWidget* find_btn = gtk_button_new_with_label("Find");

    gtk_box_pack_start(GTK_BOX(content_area), entry, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(content_area), find_btn, FALSE, FALSE, 5);

    g_signal_connect(find_btn, "clicked", G_CALLBACK(on_find_clicked), entry);

    gtk_widget_show_all(find_friend_dialog);
    gtk_dialog_run(GTK_DIALOG(find_friend_dialog));
    // gtk_widget_destroy(find_friend_dialog);
}

// Create Room 버튼 콜백
void on_create_room(GtkButton* button, gpointer user_data) {
	GtkWindow* parent = GTK_WINDOW(user_data);
	GtkWidget* dialog = gtk_dialog_new_with_buttons(
		"Create Room",
		parent,
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		"_Create", GTK_RESPONSE_OK,
		"_Cancel", GTK_RESPONSE_CANCEL,
		NULL
	);
	GtkWidget* content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	GtkWidget* entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Enter room name");
	gtk_box_pack_start(GTK_BOX(content_area), entry, FALSE, FALSE, 5);
	gtk_widget_show_all(dialog);
	gint response = gtk_dialog_run(GTK_DIALOG(dialog));
	if (response == GTK_RESPONSE_OK) {
		const char* room_name = gtk_entry_get_text(GTK_ENTRY(entry));
		if (strlen(room_name) > 0) {
			// 서버에 방 생성 요청
			send_message(TYPE_CREATE, room_name);
		}
		else {
			g_idle_add(append_message_to_view_idle, g_strdup("[Client] Room name cannot be empty."));
		}
	}
	gtk_widget_destroy(dialog);
}

// 1:1 채팅방 생성 콜백
void on_start_chat_clicked(GtkWidget* widget, gpointer data) {
    const char* friend_name = (const char*)data;
    printf("Start 1:1 chat with %s\n", friend_name);
    // 1:1 채팅방 생성 로직 구현
}

// 친구 삭제 콜백
void on_remove_friend_clicked(GtkWidget* widget, gpointer data) {
    const char* friend_name = (const char*)data;
    printf("Remove friend: %s\n", friend_name);
    // 친구 삭제 로직 구현
	send_message(TYPE_REMOVE_FRIEND, friend_name); // 서버에 친구 삭제 요청
}

// 친구 프로필 윈도우 생성 함수 예시 (실제로는 직접 구현 필요)
void on_friend_button_clicked(GtkWidget* widget, gpointer data) {
    const char* friend_name = (const char*)data;

    // 다이얼로그(팝업) 생성
    GtkWidget* dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "친구 프로필");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);

    // 메인 컨텐츠 영역 가져오기
    GtkWidget* content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    // 프로필 이미지 (기본 아이콘 사용)
    GtkWidget* image = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_DIALOG);
    gtk_box_pack_start(GTK_BOX(content_area), image, FALSE, FALSE, 5);

    // 친구 이름 라벨
    GtkWidget* name_label = gtk_label_new(friend_name);
    gtk_box_pack_start(GTK_BOX(content_area), name_label, FALSE, FALSE, 5);

    // 버튼 박스 생성
    GtkWidget* button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    // Start Chat 버튼
    GtkWidget* chat_button = gtk_button_new_with_label("Start Chat");
    g_signal_connect(chat_button, "clicked", G_CALLBACK(on_start_chat_clicked), (gpointer)friend_name);
    gtk_box_pack_start(GTK_BOX(button_box), chat_button, TRUE, TRUE, 5);

    // Remove Friend 버튼
    GtkWidget* remove_button = gtk_button_new_with_label("Remove Friend");
    g_signal_connect(remove_button, "clicked", G_CALLBACK(on_remove_friend_clicked), (gpointer)friend_name);
    gtk_box_pack_start(GTK_BOX(button_box), remove_button, TRUE, TRUE, 5);

    // 버튼 박스 추가
    gtk_box_pack_start(GTK_BOX(content_area), button_box, FALSE, FALSE, 5);

    gtk_widget_show_all(dialog);
}

// 채팅방 윈도우 생성 함수 예시 (실제로는 직접 구현 필요)
void on_chat_button_clicked(GtkWidget* widget, gpointer data) {
    const char* room_name = (const char*)data;
	printf("Open chat room: %s\n", room_name);
	send_message(TYPE_JOIN, room_name); // 서버에 채팅방 참여 요청
}

// 친구탭 UI 생성
GtkWidget* create_friends_tab() {
    friend_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    for (int i = 0; i < num_friends; i++) {
        GtkWidget* button = gtk_button_new();
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        GtkWidget* icon = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_DIALOG);
        GtkWidget* label = gtk_label_new(friends[i]);
        gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(button), box);
        gtk_box_pack_start(GTK_BOX(friend_tab), button, FALSE, FALSE, 0);

		// 버튼 클릭 시 친구 프로필 윈도우 생성 함수 호출
		g_signal_connect(button, "clicked", G_CALLBACK(on_friend_button_clicked), (gpointer)friends[i]);
    }
    gtk_widget_set_halign(friend_tab, GTK_ALIGN_START);
    return friend_tab;
}

// 채팅탭 UI 생성
GtkWidget* create_chats_tab() {
    chat_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    for (int i = 0; i < num_rooms; i++) {
        GtkWidget* button = gtk_button_new();
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        GtkWidget* label = gtk_label_new(rooms[i]);
        char member_str[32];
        snprintf(member_str, sizeof(member_str), "Members: %d", members[i]);
        GtkWidget* member_label = gtk_label_new(member_str);
        gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), member_label, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(button), box);
        gtk_box_pack_start(GTK_BOX(chat_tab), button, FALSE, FALSE, 0);

        // 버튼 클릭 시 채팅방 윈도우 생성 함수 호출
        g_signal_connect(button, "clicked", G_CALLBACK(on_chat_button_clicked), (gpointer)rooms[i]);
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
	// 친구 정보 서버에 요청
	send_message(TYPE_FRIENDS, "");
	// 채팅방 정보 서버에 요청
	send_message(TYPE_ROOMS, "");
    gtk_stack_set_visible_child_name(GTK_STACK(stack), tab);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(friend_tab_btn), g_strcmp0(tab, "friends") == 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chat_tab_btn), g_strcmp0(tab, "chats") == 0);
    printf("Switch\n");

    // 신호 재연결
    g_signal_handlers_unblock_by_func(friend_tab_btn, switch_tab, "friends");
    g_signal_handlers_unblock_by_func(chat_tab_btn, switch_tab, "chats");
}

// 하단 사용자 정보/버튼 UI 생성
GtkWidget* create_bottom_bar(GtkWidget* window) {
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* user_icon = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_DIALOG);
    GtkWidget* user_label = gtk_label_new(gtk_entry_get_text(GTK_ENTRY(name_entry)));
    GtkWidget* create_btn = gtk_link_button_new_with_label("Create Room", "Create Rooms"); // 실제 동작은 콜백 연결 필요
    GtkWidget* find_btn = gtk_link_button_new_with_label("Find Friend", "Find Friends");
    gtk_box_pack_start(GTK_BOX(bar), user_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), user_label, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bar), find_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bar), create_btn, FALSE, FALSE, 0);

    g_signal_connect(find_btn, "clicked", G_CALLBACK(on_find_friend), window);
	g_signal_connect(create_btn, "clicked", G_CALLBACK(on_create_room), window);
	send_message(TYPE_NICK, gtk_entry_get_text(GTK_ENTRY(name_entry))); // Send nickname to server
    return bar;
}

void on_create_vote_response(GtkDialog* dialog, gint response_id, gpointer user_data) {
    if (response_id == 1) { // Create
        // 입력값 추출 및 유효성 검사
        // send_create_vote_to_server(title, options, anonymity, duplicate);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

void on_vote_dialog_response(GtkDialog* dialog, gint response_id, gpointer user_data) {
    if (response_id == 1) { // Create New Vote
        GtkWidget* create_dialog = gtk_dialog_new_with_buttons(
            "투표 생성",
            GTK_WINDOW(dialog),
            GTK_DIALOG_MODAL,
            "Create", 1,
            "Cancel", GTK_RESPONSE_CANCEL,
            NULL
        );
        GtkWidget* content_area = gtk_dialog_get_content_area(GTK_DIALOG(create_dialog));

        // 제목 입력
        GtkWidget* title_entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(title_entry), "Title");
        gtk_box_pack_start(GTK_BOX(content_area), title_entry, FALSE, FALSE, 2);

        // 선택지 입력 (최소 2개, 최대 10개)
        GtkWidget* option_entries[10];
        for (int i = 0; i < 3; ++i) {
            option_entries[i] = gtk_entry_new();
            gtk_entry_set_placeholder_text(GTK_ENTRY(option_entries[i]), "Option");
            gtk_box_pack_start(GTK_BOX(content_area), option_entries[i], FALSE, FALSE, 2);
        }
        // + 버튼으로 선택지 추가 구현 필요


        // 익명/중복투표 체크박스
        GtkWidget* anonymity_check = gtk_check_button_new_with_label("Anonymity");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(anonymity_check), TRUE);
        gtk_box_pack_start(GTK_BOX(content_area), anonymity_check, FALSE, FALSE, 2);

        GtkWidget* duplicate_check = gtk_check_button_new_with_label("Duplicate");
        gtk_box_pack_start(GTK_BOX(content_area), duplicate_check, FALSE, FALSE, 2);

        gtk_widget_show_all(create_dialog);

        // Create 버튼 콜백 연결
        g_signal_connect(create_dialog, "response", G_CALLBACK(on_create_vote_response), NULL);
    }
}

// 투표 상세 보기 콜백
void on_vote_detail_clicked(GtkButton* button, gpointer user_data) {
    // 미구현
}

// 생성된 투표 보기
void on_vote_clicked(GtkButton* button, gpointer user_data) {

    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "투표 목록",
        NULL/*GTK_WINDOW(parent_window)*/,
        GTK_DIALOG_MODAL,
        "Create New Vote", 1,
        "Close", GTK_RESPONSE_CLOSE,
        NULL
    );
    GtkWidget* content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    // 투표 목록 동적 생성
    for (int i = 0; i < num_votes; ++i) {
        if (votes[i] == NULL) continue;
        GtkWidget* vote_btn = gtk_button_new_with_label(votes[i]->title);
        // 각 투표 상세 보기 콜백 연결
        g_signal_connect(vote_btn, "clicked", G_CALLBACK(on_vote_detail_clicked), votes[i]);
        gtk_box_pack_start(GTK_BOX(content_area), vote_btn, FALSE, FALSE, 2);
    }

    gtk_widget_show_all(dialog);

    // Create New Vote 버튼 처리
    g_signal_connect_swapped(dialog, "response", G_CALLBACK(on_vote_dialog_response), dialog);
}

void update_user_list() {
    // 기존 목록 삭제
    GList* children = gtk_container_get_children(GTK_CONTAINER(user_list));
    for (GList* iter = children; iter != NULL; iter = iter->next) {
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    }
    g_list_free(children);

    // users_in_room 배열을 기반으로 라벨 추가
    for (int i = 0; i < num_users_in_room; i++) {
        GtkWidget* user_label = gtk_label_new(g_strdup(users_in_room[i]));
        gtk_box_pack_start(GTK_BOX(user_list), user_label, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(user_list);
}

// 탭 오버레이 토글 함수
void on_tab_button_clicked(GtkButton* button, gpointer user_data) {
    GtkWidget* tab_overlay = GTK_WIDGET(user_data);
    gboolean visible = gtk_widget_get_visible(tab_overlay);
    if (visible)
        gtk_widget_hide(tab_overlay);
    else {
        update_user_list(); // 사용자 목록 갱신
        gtk_widget_show(tab_overlay);
    }

	send_message(TYPE_USERS, ""); // 서버에 현재 방의 사용자 목록 요청
}

// Send 버튼 콜백
static void on_send_clicked(gpointer data) {
    const char* text = gtk_entry_get_text(GTK_ENTRY(entry));
    if (text == NULL || strlen(text) == 0) return;

    // 서버로 메시지 전송
    send_message(TYPE_TEXT, text);

    // 입력창 비우기
    gtk_entry_set_text(GTK_ENTRY(entry), "");
}

// Exit 버튼 콜백
void on_exit_clicked(GtkButton* button, gpointer user_data) {
    send_message(TYPE_LEAVE, ""); // 서버에 방 나가기 요청
    GtkWidget* window = GTK_WIDGET(user_data);
    gtk_widget_destroy(window);
}

gboolean create_room_idle(gpointer data)
{
    const char* room_name = (const char*)data;
    printf("[DEBUG] create_room_idle called with room_name: %s\n", room_name);

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), room_name);
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 600);

    GtkWidget* overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(window), overlay);

    // 메인 VBox
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), vbox);

    // 상단 바
    GtkWidget* topbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), topbar, FALSE, FALSE, 0);

    GtkWidget* back_btn = gtk_button_new_with_label("<");
    gtk_box_pack_start(GTK_BOX(topbar), back_btn, FALSE, FALSE, 0);

    GtkWidget* tab_btn = gtk_button_new();
    GtkWidget* tab_icon = gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(tab_btn), tab_icon);
    gtk_box_pack_end(GTK_BOX(topbar), tab_btn, FALSE, FALSE, 0);

    // 채팅 메시지 영역
    GtkWidget* scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    listbox = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scrolled), listbox);

    // 하단 입력창 + 버튼
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(hbox), entry, TRUE, TRUE, 0);

    GtkWidget* send_btn = gtk_button_new_with_label("Send");
    gtk_box_pack_start(GTK_BOX(hbox), send_btn, FALSE, FALSE, 0);

    // --- 탭 오버레이 화면 ---
    GtkWidget* tab_overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(tab_overlay, 400, 400);
    gtk_widget_set_margin_top(tab_overlay, 50);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), tab_overlay);
    gtk_widget_set_halign(tab_overlay, GTK_ALIGN_FILL);
    gtk_widget_set_valign(tab_overlay, GTK_ALIGN_START);
    gtk_widget_hide(tab_overlay);

    // 좌측: 사용자 목록
    user_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_size_request(user_list, 120, -1);
    gtk_box_pack_start(GTK_BOX(tab_overlay), user_list, FALSE, FALSE, 0);

    GtkWidget* user1 = gtk_label_new("Lilly");
    GtkWidget* user2 = gtk_label_new("Marco");
    GtkWidget* user3 = gtk_label_new("Steve");
    gtk_box_pack_start(GTK_BOX(user_list), user1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(user_list), user2, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(user_list), user3, FALSE, FALSE, 0);

    // 우측: 메뉴 버튼
    GtkWidget* menu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_box_pack_start(GTK_BOX(tab_overlay), menu_box, TRUE, TRUE, 0);

    GtkWidget* invite_btn = gtk_button_new_with_label("Invite Friend");
    GtkWidget* vote_btn = gtk_button_new_with_label("Vote");
    GtkWidget* chess_btn = gtk_button_new_with_label("Chess Game");
    GtkWidget* exit_btn = gtk_button_new_with_label("Exit");
    gtk_box_pack_start(GTK_BOX(menu_box), invite_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(menu_box), vote_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(menu_box), chess_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(menu_box), exit_btn, FALSE, FALSE, 0);

    // 콜백 연결
    g_signal_connect(tab_btn, "clicked", G_CALLBACK(on_tab_button_clicked), tab_overlay);
    g_signal_connect(exit_btn, "clicked", G_CALLBACK(on_exit_clicked), window);
    g_signal_connect(send_btn, "clicked", G_CALLBACK(on_send_clicked), NULL);

	g_signal_connect(vote_btn, "clicked", G_CALLBACK(on_vote_clicked), NULL);

    gtk_widget_show_all(window);
    gtk_widget_hide(tab_overlay); // 시작 시 탭 화면 숨김
}

gboolean cmd_text_idle(gpointer data) {
	char* message = (char*)data;
	char* sender_name = strtok(message, "\n"); // 메시지에서 이름 추출
	char* text_message = strtok(NULL, "\n"); // 메시지 내용 추출
	printf("[DEBUG] Sender: %s , Message : %s\n", sender_name, text_message);

	if (sender_name == NULL || text_message == NULL) {
		printf("[ERROR] Invalid message format: %s\n", message);
		// g_free(message);
		return G_SOURCE_REMOVE; // Remove source after execution
	}

    // 메시지 행을 감쌀 박스 생성
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

    // 프로필(아이콘) 생성
    GtkWidget* profile = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_DIALOG);
    gtk_box_pack_start(GTK_BOX(row), profile, FALSE, FALSE, 0);

    // 이름+메시지 수직 박스
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    GtkWidget* name_label = gtk_label_new(g_strdup(sender_name)); // 실제로는 패킷에서 이름 파싱 필요
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), name_label, FALSE, FALSE, 0);

    GtkWidget* msg_label = gtk_label_new(g_strdup(text_message));
    gtk_label_set_xalign(GTK_LABEL(msg_label), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), msg_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(row), vbox, TRUE, TRUE, 0);

    // listbox에 추가
    gtk_list_box_insert(GTK_LIST_BOX(listbox), row, -1);
    gtk_widget_show_all(row);
	gtk_widget_show_all(listbox); // listbox 업데이트
	g_free(message); // Free the message string
}

void cmd_friends(char* message) {
	num_friends = 0; // Reset friend count

    // 첫 줄("Friends:")은 무시하고, 이후 줄을 닉네임으로 저장
    char* line = strtok(message, "\n");
    if (line && strcmp(line, "Friends:") == 0) {
        line = strtok(NULL, "\n"); // 다음 줄로 이동
        while (line != NULL && num_friends < 20) {
            friends[num_friends] = strdup(line); // 닉네임 복사
            num_friends++;
            line = strtok(NULL, "\n");
        }
    }

    // 이후 사용 예시
    for (int i = 0; i < num_friends; i++) {
        printf("%d: %s\n", i, friends[i]);
    }
}

void cmd_add_friend(const char* message, const char* name) {
	if (message == NULL || strlen(message) == 0) {
		g_idle_add(append_message_to_view_idle, g_strdup("[Client] Usage: /addfriend <nickname>"));
		return;
	}
	else printf("[DEBUG] cmd_add_friend called with message: %s\n", message);

	// message가 "Failed"이면 친구 추가 실패
	if (strcmp(message, "Failed") == 0) {
		g_idle_add(show_no_user_dialog_idle, g_strdup(name)); // 친구가 없을 때
        //show_no_user_dialog(name);
		return;
	}
	// message가 "Succeed"이면 친구 추가 성공
	if (strcmp(message, "Succeed") == 0) {
		g_idle_add(show_added_friend_dialog_idle, g_strdup(name)); // 친구 추가 성공
		// show_added_friend_dialog(name);
		return;
	}
}

// 채팅방 생성 함수
void cmd_create_room(const char* room_name) {
	printf("[DEBUG] cmd_create_room called with room_name: %s\n", room_name);
	// 방 생성 완료 후 UI 업데이트
	g_idle_add(create_room_idle, g_strdup(room_name));
}

// 서버로부터 room 정보를 받음
void cmd_rooms(char* message) {
    num_rooms = 0;

	// 첫 줄("Rooms:")은 무시하고, 이후 줄을 방 이름과 멤버 수로 저장
	// 방 이름과 멤버 수는 공백으로 구분
    char* line = strtok(message, "\n");
    if (line && strcmp(line, "Rooms:") == 0) {
        line = strtok(NULL, "\n"); // 첫 방 정보로 이동
        while (line != NULL && num_rooms < 20) {
            // 한 줄에서 방 이름과 멤버 수 분리 (strtok 사용 X)
            char room_name[128];
            int member_count;
            if (sscanf(line, "%127s %d", room_name, &member_count) == 2) {
                rooms[num_rooms] = strdup(room_name);
                members[num_rooms] = member_count;
                num_rooms++;
                printf("[DEBUG] Room %d: %s (Members: %d)\n", num_rooms - 1, rooms[num_rooms - 1], members[num_rooms - 1]);
            }
            else {
                printf("[ERROR] Invalid room format: %s\n", line);
            }
            line = strtok(NULL, "\n"); // 다음 줄로 이동
        }
    }

    // 이후 사용 예시
	for (int i = 0; i < num_rooms; i++) {
		printf("%d: %s (Members: %d)\n", i, rooms[i], members[i]);
	}
}

void cmd_join_room(const char* room_name) {
	printf("[DEBUG] cmd_join_room called with room_name: %s\n", room_name);
	// 채팅방 참여 완료 후 UI 업데이트
	g_idle_add(create_room_idle, g_strdup(room_name));
}

// 서버로부터 입력받은 메시지를 채팅창에 추가하는 함수
void cmd_text(const char* message) {
	if (message == NULL || strlen(message) == 0) {
		printf("[DEBUG] cmd_text called with empty message.\n");
		return;
	}
	// 메시지를 채팅창에 추가
	g_idle_add(cmd_text_idle, g_strdup(message));
}

// 서버로부터 채팅방 사용자 목록을 받음
void cmd_users(char* message) {
	num_users_in_room = 0; // Reset user count
	// 첫 줄("Users:")은 무시하고, 이후 줄을 사용자 이름으로 저장
	char* line = strtok(message, "\n");
	if (line && strcmp(line, "Users:") == 0) {
		line = strtok(NULL, "\n"); // 다음 줄로 이동
		while (line != NULL && num_users_in_room < 20) {
			users_in_room[num_users_in_room] = strdup(line); // 사용자 이름 복사
			num_users_in_room++;
			line = strtok(NULL, "\n");
		}
	}
}

// Thread function to receive messages from the server
static void* receive_messages(void* arg) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    while ((bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                printf("[INFO] Server disconnected.\n");
            }
            else {
                // EAGAIN/EWOULDBLOCK might occur if RCVTIMEO was set and expired.
                // Otherwise, it's a more serious error.
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("recv error");
                    printf("[ERROR] Recv error on Server (fd %d).\n", sock);
                }
            }
            break;
        }
        buffer[bytes_received] = '\0';
        buffer[strcspn(buffer, "\n")] = 0; // Remove trailing newline

        if (bytes_received < sizeof(PacketHeader)) {
            printf("[Debug] Received packet too small: %zd bytes\n", bytes_received);
            continue;
        }
		// Extract packet header
		PacketHeader header;
		memcpy(&header, buffer, sizeof(PacketHeader));
		printf("[DEBUG] Received packet type: %d, length: %d\n", header.type, header.length);

        char* message_start = buffer + sizeof(PacketHeader);
        if (header.length > 0)
        {
            // Ensure the message is null-terminated
            message_start[header.length] = '\0';
        }
        else {
            // Invalid length, skip processing
            printf("[Server] Invalid message length.\n");
            continue;
        }

		// Process the received message based on its type
        switch (header.type)
        {
        case TYPE_TEXT:
			cmd_text(message_start); // 채팅 메시지 처리
            break;
        case TYPE_NICK:
            break;
        case TYPE_ROOMS:
			cmd_rooms(message_start); // 채팅방 목록 요청에 대한 서버의 응답 처리
            break;
        case TYPE_CREATE:
			cmd_create_room(message_start); // 채팅방 생성 요청에 대한 서버의 응답 처리
            break;
		case TYPE_JOIN:
			cmd_join_room(message_start); // 채팅방 참여 요청에 대한 서버의 응답 처리
            break;
		case TYPE_LEAVE:
			break;
		case TYPE_USERS:
			cmd_users(message_start); // 채팅방 사용자 목록 요청에 대한 서버의 응답 처리
			break;
		case TYPE_FRIENDS:
			cmd_friends(message_start); // 친구 목록 요청에 대한 서버의 응답 처리
            break;
		case TYPE_CHANGE:
			break;
		case TYPE_KICK:
			break;
		case TYPE_ADD_FRIEND: // 친구 추가 완료에 대한 서버의 ACK
			char* message = strtok(message_start, " ");
			char* name = strtok(NULL, " ");
			cmd_add_friend(message_start, name);
            break;
		case TYPE_REMOVE_FRIEND: // 친구 삭제 완료에 대한 서버의 ACK
            break;
		case TYPE_VOTES:
            break;
        case TYPE_CREATE_VOTE:
            break;
        case TYPE_DO_VOTE:
            break;
		case TYPE_VOTE_RESULT:
            break;
		case TYPE_ERROR:
			printf("[Server] Error: %s\n", message_start);
            break;
		default:
            break;
        }
    }

    if (bytes_received == 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Disconnected from server."));
    }
    else if (bytes_received < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error receiving message."));
    }

    // Safely update UI elements (disable sending, enable connect)
    WidgetSensitivityData* send_data = g_new(WidgetSensitivityData, 1);
    send_data->widget = send_button; send_data->sensitive = FALSE;
    g_idle_add(set_widget_sensitive_idle, send_data);
    WidgetSensitivityData* msg_data = g_new(WidgetSensitivityData, 1);
    msg_data->widget = entry; msg_data->sensitive = FALSE;
    g_idle_add(set_widget_sensitive_idle, msg_data);
    WidgetSensitivityData* conn_data = g_new(WidgetSensitivityData, 1);
    conn_data->widget = connect_button; conn_data->sensitive = TRUE;
    g_idle_add(set_widget_sensitive_idle, conn_data);

    if (sock != -1) {
        close(sock);
        sock = -1;
    }
    recv_thread_id = 0; // Mark thread as finished
    return NULL;
}

// Callback for the "Enter" button
static void on_enter_button_clicked(GtkWidget *widget, gpointer data) {
    if (sock != -1) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Already connected or attempting to connect."));
        return;
    }
    const char* ip_address = gtk_entry_get_text(GTK_ENTRY(ip_entry));
    const char* port_str = gtk_entry_get_text(GTK_ENTRY(port_entry));
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

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Connection failed."));
        close(sock); sock = -1; return; // Close socket on connection failure
    }

    g_idle_add(append_message_to_view_idle, g_strdup("[Client] Connected to server!"));

    if (pthread_create(&recv_thread_id, NULL, receive_messages, NULL) != 0) {
        perror("pthread_create for receive_messages");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Failed to create receive thread."));
        close(sock); sock = -1;
    }
    
    // 메인 윈도우 생성
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
        GtkWidget* bottom_bar = create_bottom_bar(main_window);

        gtk_box_pack_start(GTK_BOX(main_box), tab_bar, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(main_box), stack, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(main_box), bottom_bar, FALSE, FALSE, 0);

        gtk_container_add(GTK_CONTAINER(main_window), main_box);

        // 탭 버튼 콜백 연결
        g_signal_connect(friend_tab_btn, "clicked", G_CALLBACK(switch_tab), "friends");
        g_signal_connect(chat_tab_btn, "clicked", G_CALLBACK(switch_tab), "chats");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(friend_tab_btn), TRUE);
        gtk_widget_show_all(main_window);
    }
    else {
        // 이미 열려 있다면 포커스만 줍니다 (선택적)
        gtk_window_present(GTK_WINDOW(main_window));
    }
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
