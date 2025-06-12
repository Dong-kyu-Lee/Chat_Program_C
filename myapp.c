#include <gtk/gtk.h>

const char* friends[] = { "Lilly", "Marco", "Father" };
const int num_friends = 3;

struct Room {
    const char* name;
    int members;
};
struct Room rooms[] = {
    { "Room Name", 4 },
    { "Room Name 1", 2 },
    { "Room Name 2", 5 }
};
const int num_rooms = 3;

GtkWidget* create_friend_tab() {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    for (int i = 0; i < num_friends; ++i) {
        GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

        GtkWidget* icon = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_DIALOG);
        gtk_box_pack_start(GTK_BOX(hbox), icon, FALSE, FALSE, 0);

        GtkWidget* label = gtk_label_new(friends[i]);
        gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 10);

        GtkWidget* chat_icon = gtk_image_new_from_icon_name("mail-message-new", GTK_ICON_SIZE_MENU);
        gtk_box_pack_end(GTK_BOX(hbox), chat_icon, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(vbox);
    return vbox;
}

GtkWidget* create_chat_tab() {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    for (int i = 0; i < num_rooms; ++i) {
        GtkWidget* frame = gtk_frame_new(NULL);
        gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_OUT);

        GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget* room_label = gtk_label_new(rooms[i].name);

        char member_str[32];
        snprintf(member_str, sizeof(member_str), "Members : %d", rooms[i].members);
        GtkWidget* member_label = gtk_label_new(member_str);

        gtk_box_pack_start(GTK_BOX(hbox), room_label, FALSE, FALSE, 10);
        gtk_box_pack_end(GTK_BOX(hbox), member_label, FALSE, FALSE, 10);

        gtk_container_add(GTK_CONTAINER(frame), hbox);
        gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(vbox);
    return vbox;
}

GtkWidget* create_bottom_bar() {
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget* icon = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_DIALOG);
    gtk_box_pack_start(GTK_BOX(hbox), icon, FALSE, FALSE, 0);
    GtkWidget* label = gtk_label_new("John");
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

    GtkWidget* btn_create = gtk_button_new_with_label("Create Room");
    GtkWidget* btn_find = gtk_button_new_with_label("Find Friend");
    gtk_box_pack_end(GTK_BOX(hbox), btn_find, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), btn_create, FALSE, FALSE, 0);

    gtk_widget_show_all(hbox);
    return hbox;
}

void create_main_window() {
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "�ㅽ뻾�붾㈃");
    gtk_window_set_default_size(GTK_WINDOW(window), 350, 500);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget* tab_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget* btn_friend = gtk_button_new();
    GtkWidget* btn_chat = gtk_button_new();
    GtkWidget* img_friend = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_DIALOG);
    GtkWidget* img_chat = gtk_image_new_from_icon_name("mail-message-new", GTK_ICON_SIZE_DIALOG);
    gtk_button_set_image(GTK_BUTTON(btn_friend), img_friend);
    gtk_button_set_image(GTK_BUTTON(btn_chat), img_chat);
    gtk_box_pack_start(GTK_BOX(tab_hbox), btn_friend, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(tab_hbox), btn_chat, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), tab_hbox, FALSE, FALSE, 0);

    GtkWidget* notebook = gtk_notebook_new();
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);

    GtkWidget* friend_tab = create_friend_tab();
    GtkWidget* chat_tab = create_chat_tab();

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), friend_tab, NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), chat_tab, NULL);

    GtkWidget* bottom_bar = create_bottom_bar();
    gtk_box_pack_end(GTK_BOX(vbox), bottom_bar, FALSE, FALSE, 0);

    g_signal_connect(btn_friend, "clicked", G_CALLBACK(gtk_notebook_set_current_page), notebook);
    g_signal_connect_swapped(btn_friend, "clicked", G_CALLBACK(gtk_notebook_set_current_page), notebook);
    g_signal_connect(btn_chat, "clicked", G_CALLBACK(gtk_notebook_set_current_page), notebook);

    g_signal_connect(btn_friend, "clicked", G_CALLBACK(gtk_notebook_set_current_page), GINT_TO_POINTER(0));
    g_signal_connect(btn_chat, "clicked", G_CALLBACK(gtk_notebook_set_current_page), GINT_TO_POINTER(1));

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(window);
}

void on_enter_clicked(GtkWidget* widget, gpointer window) {
    gtk_widget_destroy(GTK_WIDGET(window));
    create_main_window();
}

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "�ㅽ뻾�붾㈃ - �낆옣");
    gtk_window_set_default_size(GTK_WINDOW(window), 350, 180);
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_add(GTK_CONTAINER(window), grid);

    GtkWidget* label_ip = gtk_label_new("IP:");
    GtkWidget* entry_ip = gtk_entry_new();
    gtk_grid_attach(GTK_GRID(grid), label_ip, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry_ip, 1, 0, 2, 1);

    GtkWidget* label_port = gtk_label_new("Port:");
    GtkWidget* entry_port = gtk_entry_new();
    gtk_grid_attach(GTK_GRID(grid), label_port, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry_port, 1, 1, 2, 1);

    GtkWidget* label_name = gtk_label_new("Name:");
    GtkWidget* entry_name = gtk_entry_new();
    gtk_grid_attach(GTK_GRID(grid), label_name, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry_name, 1, 2, 2, 1);

    GtkWidget* button_enter = gtk_button_new_with_label("Enter");
    gtk_grid_attach(GTK_GRID(grid), button_enter, 2, 3, 1, 1);

    g_signal_connect(button_enter, "clicked", G_CALLBACK(on_enter_clicked), window);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}