#pragma once
#include <string>

void send_username();
void start_as_host();
void start_as_guest(const std::string& code);
void game_tick();
void load_server_list(const std::string& raw_pastebin_text);
void fetch_and_load_servers(const std::string& url);
void process_pending_server_list();