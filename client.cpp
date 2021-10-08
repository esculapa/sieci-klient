#include <iostream>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <sys/timerfd.h>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <stdlib.h>
#include <endian.h>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/time.h>

#include "err.h"
#include "my_rand.h"
#include "crc32.h"

#define MAX_PLAYER_NAME_SIZE 20
#define MAX_NUMBER_OF_PLAYERS 25

#define SERVER_DESCRIPTOR 0
#define GUI_DECSCRIPTOR 1
#define TIMER_DESCRIPTOR 2
#define NUMBER_OF_DESCRIPTORS 3
#define LEFT 2
#define RIGHT 1
#define BUFFERSIZE 550
#define MESSAGESIZE 40
#define COORDINATES_VALUE_SIZE 9 // space in vector needed for two coordinates values in buffer
#define MILION 1000*1000
#define TIME_INTERVAL 30 * MILION
#define FLAGS 0
#define BYTES4 4
#define BYTE1 1

std::string player_name, server_name = "localhost", gui_address = "localhost";
uint16_t port_server = 2021, port_gui = 20210;
uint64_t session_id;
uint32_t game_id = 0;
uint8_t turn_direction = 0;
uint32_t next_expected_event_no = 0, global_maxx, global_maxy;
std::string previous_buffer_gui, previous_buffer_server;
std::string player_names[MAX_NUMBER_OF_PLAYERS];
bool game_is_on = false;

struct pollfd fds[NUMBER_OF_DESCRIPTORS];
struct sockaddr_in server_address;

bool is_player_name_correct() {
  if (player_name.size() > MAX_PLAYER_NAME_SIZE)
    return false;
  for (size_t i = 0; i < player_name.size(); i++) {
    if (player_name[i] >= 33 && player_name[i] <= 126)
      continue;
    return false;
  }
  return true;
}

void initalize_socks() {
  int err;
  /* server connection */
  struct addrinfo addr_hints_server;
  struct addrinfo *addr_result_server;

  // 'converting' host/port in string to struct addrinfo
  (void) memset(&addr_hints_server, 0, sizeof(struct addrinfo));
  addr_hints_server.ai_family = AF_UNSPEC;
  addr_hints_server.ai_socktype = SOCK_DGRAM;
  addr_hints_server.ai_protocol = IPPROTO_UDP;
  addr_hints_server.ai_flags = 0;
  addr_hints_server.ai_addrlen = 0;
  addr_hints_server.ai_addr = NULL;
  addr_hints_server.ai_canonname = NULL;
  addr_hints_server.ai_next = NULL;
  if (getaddrinfo(server_name.c_str(), (std::to_string(port_server)).c_str(),
      &addr_hints_server, &addr_result_server) != 0) {
    syserr("getaddrinfo server");
  }

  fds[SERVER_DESCRIPTOR].fd = socket(addr_result_server->ai_family, addr_result_server->ai_socktype, addr_result_server->ai_protocol);
  fds[SERVER_DESCRIPTOR].events = POLLIN | POLLERR;
  fds[SERVER_DESCRIPTOR].revents = 0;

  server_address.sin_family = AF_UNSPEC;
  server_address.sin_addr.s_addr =
    ((struct sockaddr_in*) (addr_result_server->ai_addr))->sin_addr.s_addr; // address IP
  server_address.sin_port = htons(port_server); // port from the command line

  freeaddrinfo(addr_result_server);

  /* connecting gui */
  struct addrinfo addr_hints_gui;
  struct addrinfo *addr_result_gui;

  memset(&addr_hints_gui, 0, sizeof(struct addrinfo));
  addr_hints_gui.ai_family = AF_UNSPEC;
  addr_hints_gui.ai_socktype = SOCK_STREAM;
  addr_hints_gui.ai_protocol = IPPROTO_TCP;
  err = getaddrinfo(gui_address.c_str(), (std::to_string(port_gui)).c_str(), &addr_hints_gui, &addr_result_gui);
  if (err == EAI_SYSTEM) { // system error
    syserr("getaddrinfo: %s", gai_strerror(err));
  }
  else if (err != 0) { // other error (host not found, etc.)
    fatal("getaddrinfo: %s", gai_strerror(err));
  }
  // initialize socket according to getaddrinfo results
  int sock = socket(addr_result_gui->ai_family, addr_result_gui->ai_socktype, addr_result_gui->ai_protocol);
  if (sock < 0)
    syserr("socket gui");

  const int one = 1;
  if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one))) {
    fatal("Disabling Nagle algorithm");
  }

  // connect socket to the server
  if (connect(sock, addr_result_gui->ai_addr, addr_result_gui->ai_addrlen) < 0)
    syserr("connect gui");

  fds[GUI_DECSCRIPTOR].fd = sock;
  fds[GUI_DECSCRIPTOR].events = POLLIN | POLLERR;
  fds[GUI_DECSCRIPTOR].revents = 0;

  previous_buffer_gui = "";


  /* timer setting */
  fds[TIMER_DESCRIPTOR].fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  fds[TIMER_DESCRIPTOR].events = POLLIN | POLLERR;
  fds[TIMER_DESCRIPTOR].revents = 0;

  struct itimerspec timer = { {0, TIME_INTERVAL}, {0, TIME_INTERVAL}};
  int timer_creation = timerfd_settime(fds[TIMER_DESCRIPTOR].fd, 0,
                           &timer, NULL);
  if (timer_creation != 0) {
    fatal("Timer creation");
  }
}

void send_message_to_game_server() {
  ssize_t current_length = 0;
  char message[MESSAGESIZE];
  uint64_t session_id_mesg = htobe64(session_id);
  std::memcpy(message, &session_id_mesg, 8);
  current_length += 8;

  uint8_t turn_direction_mesg = turn_direction;
  std::memcpy(message + current_length, &turn_direction_mesg, BYTE1);
  current_length += BYTE1;

  uint32_t next_expected_event_no_mesg = htobe32(next_expected_event_no);
  std::memcpy(message + current_length, &next_expected_event_no_mesg, BYTES4);
  current_length += BYTES4;

  std::memcpy(message + current_length, &player_name[0], player_name.size());

  current_length += player_name.size();

  int snd_len = sendto(fds[SERVER_DESCRIPTOR].fd, message, current_length,
    FLAGS, (const struct sockaddr*)&server_address, sizeof(server_address));
  if (snd_len != current_length) {
    fatal("Send error");
  }
}

void send_to_gui(std::string message) {
  int snd_len = write(fds[GUI_DECSCRIPTOR].fd, &message[0], message.size());
  if (snd_len !=((int) message.size())) {
    fatal("Send to gui error");
  }
}

void handle_new_game(uint32_t game_id_mesg, uint32_t event_no, uint32_t maxx, uint32_t maxy, std::string player_names_list) {
  /* settings */
  next_expected_event_no = 0;
  global_maxx = maxx;
  global_maxy = maxy;
  if (event_no != next_expected_event_no)
    return;
  next_expected_event_no++;
  game_is_on = true;
  game_id = game_id_mesg;

  if (player_names_list.size() < 2) {
    fatal("Incorrect input data from server");
  }

  size_t added_player_number = 0;
  player_names[added_player_number] = "";
  for (size_t i = 0; i < player_names_list.size(); i++) {
    if (player_names_list[i] == '\0') {
      added_player_number++;
      player_names[added_player_number] = "";
      continue;
    }
    player_names[added_player_number] = player_names[added_player_number] + player_names_list[i];
  }

  /* message to gui */
  std::string buffer = "NEW_GAME " + std::to_string(maxx) + " " +
    std::to_string(maxy);
  for (size_t i = 0; i < added_player_number; i++) {
    buffer += ' ' + player_names[i];
  }
  buffer += "\n";

  send_to_gui(buffer);
}

void handle_pixel(uint32_t game_id_mesg, uint32_t event_no, uint8_t player_number, uint32_t x, uint32_t y) {
  if (player_number >= MAX_NUMBER_OF_PLAYERS || x > global_maxx || y > global_maxy)
    fatal("Incorrect data from server");

  if (event_no != next_expected_event_no || !game_is_on || game_id != game_id_mesg){
    return;
  }
  next_expected_event_no++;

  /* message to gui */
  std::string buffer = "PIXEL " + std::to_string(x) + " " + std::to_string(y) + " " +
    player_names[player_number] + "\n";
  send_to_gui(buffer);
}

void handle_player_eliminated(uint32_t game_id_mesg, uint32_t event_no, uint8_t player_number) {
  if (player_number >= MAX_NUMBER_OF_PLAYERS)
    fatal("Incorrect data from server");
  if (event_no != next_expected_event_no || !game_is_on || game_id != game_id_mesg){
    return;
  }
  next_expected_event_no++;

  /* message to gui */
  send_to_gui("PLAYER_ELIMINATED " + player_names[player_number] + "\n");
}

void handle_game_over(uint32_t game_id_mesg, uint32_t event_no) {
  if (event_no != next_expected_event_no){
    return;
  }
  next_expected_event_no++;

  if (!game_is_on || game_id_mesg != game_id)
    return;

  game_is_on = false;
  return;
}

void read_from_server() {
  unsigned char buffer[BUFFERSIZE];
  int current_length = 0;

  int rcv_len = read(fds[SERVER_DESCRIPTOR].fd, buffer, BUFFERSIZE);
  if (rcv_len < 0)
    fatal("Read Server Error");

  uint32_t game_id_mesg = 0;
  std::memcpy(&game_id_mesg, buffer + current_length, BYTES4);
  current_length += BYTES4;

  game_id_mesg = be32toh(game_id_mesg);

  int start_new_events = current_length;

  while (current_length < rcv_len) {
    start_new_events = current_length;
    uint32_t len = 0;
    std::memcpy(&len, buffer + current_length, BYTES4);

    current_length += BYTES4;
    len = be32toh(len);

    uint32_t event_no_mesg;
    std::memcpy(&event_no_mesg, buffer + current_length, BYTES4);
    event_no_mesg = be32toh(event_no_mesg);
    current_length += BYTES4;

    uint8_t event_type_mesg;
    std::memcpy(&event_type_mesg, buffer + current_length, BYTE1);
    current_length += BYTE1;

    uint32_t maxx_mesg = 0, maxy_mesg = 0, x_mesg = 0, y_mesg = 0;
    int8_t player_number_mesg = 0;
    std::string player_list_mesg = "";

    switch (event_type_mesg) {
      case 0: // NEW_GAME
        std::memcpy(&maxx_mesg, buffer + current_length, BYTES4);
        maxx_mesg = be32toh(maxx_mesg);
        current_length += BYTES4;

        std::memcpy(&maxy_mesg, buffer + current_length, BYTES4);
        maxy_mesg = be32toh(maxy_mesg);
        current_length += BYTES4;

        player_list_mesg.resize(len - 3 * BYTES4 - BYTE1);
        std::memcpy(&player_list_mesg[0], buffer + current_length, player_list_mesg.size());
        current_length += player_list_mesg.size();
        break;
      case 1: // PIXEL
        std::memcpy(&player_number_mesg, buffer + current_length, BYTE1);
        current_length += BYTE1;

        std::memcpy(&x_mesg, buffer + current_length, BYTES4);
        x_mesg = be32toh(x_mesg);
        current_length += BYTES4;

        std::memcpy(&y_mesg, buffer + current_length, BYTES4);
        y_mesg = be32toh(y_mesg);
        current_length += BYTES4;
        break;
      case 2: // PLAYER_ELIMINATED
        std::memcpy(&player_number_mesg, buffer + current_length, 1);
        current_length += BYTE1;
        break;
      case 3: // GAME_OVER
        break;
      default:
        break;
    }

    if (!crc32(buffer + start_new_events, current_length - start_new_events)) {
      return;
    }
    current_length += 4;

    switch(event_type_mesg) {
      case 0: // NEW_GAME
        handle_new_game(game_id_mesg, event_no_mesg, maxx_mesg, maxy_mesg, player_list_mesg);
        break;
      case 1: // PIXEL
        handle_pixel(game_id_mesg, event_no_mesg, player_number_mesg, x_mesg, y_mesg);
        break;
      case 2: // PLAYER_ELIMINATED
        handle_player_eliminated(game_id_mesg, event_no_mesg, player_number_mesg);
        break;
      case 3: // GAME_OVER
        handle_game_over(game_id_mesg, event_no_mesg);
        break;
      default:
        break;
    }
  }
}

void read_from_gui() {
  char buffer_to_read[BUFFERSIZE];
  int rcv_len = read(fds[GUI_DECSCRIPTOR].fd, buffer_to_read, BUFFERSIZE);
  if (rcv_len < 0){
    syserr("Read Gui Error");
  }

  std::string buffer(buffer_to_read);
  std::string command = previous_buffer_gui;

  for (int i = 0; i < rcv_len; i++) {
    if (buffer[i] != '\n') {
      command = command + buffer[i];
      continue;
    }
    if (command == "RIGHT_KEY_DOWN") {
          turn_direction &= RIGHT;
    }
    if (command == "RIGHT_KEY_UP") {
          turn_direction ^= RIGHT;
    }
    if (command == "LEFT_KEY_DOWN") {
          turn_direction &= LEFT;
    }
    if (command == "LEFT_KEY_UP") {
          turn_direction ^= LEFT;
    }
    command = "";
  }
  previous_buffer_gui = command;
}

void play() {
  while(true) {
    int situation =  poll(fds, NUMBER_OF_DESCRIPTORS, -1);
    if (situation == -1) {
      fatal("Communication error");
    }
    for (int descriptor_number = NUMBER_OF_DESCRIPTORS - 1;
        descriptor_number >= 0; descriptor_number--) {
      if (fds[descriptor_number].revents & (POLLIN | POLLERR)) {
        if (descriptor_number == TIMER_DESCRIPTOR) {
          uint64_t number_read;
          int correct_read = read(fds[descriptor_number].fd,
            &number_read, sizeof(uint64_t));
          if(correct_read == -1) {
            fatal("read timer failure");
          }
          send_message_to_game_server();
        }
        if (descriptor_number == SERVER_DESCRIPTOR) {
          read_from_server();
        }
        if (descriptor_number == GUI_DECSCRIPTOR) {
          read_from_gui();
        }
      }
    }
  }
}

void set_session_id() {
  struct timeval t;
  struct timezone tz;
  int time_get = gettimeofday(&t, &tz);
  if (time_get)
    fatal("Setting time");
  session_id = t.tv_sec * MILION + t.tv_usec;
}

int main(int argc, char *argv[]) {

  set_session_id();
  int opt;
  if (argc < 1) {
    fatal("Not enough number of arguments");
  }
  server_name = argv[1];
  while ((opt = getopt(argc, argv, "n:p:i:r:")) != -1) {
          switch (opt) {
          case 'n':
              player_name = optarg;
              break;
          case 'p':
              port_server = atoi(optarg);
              break;
          case 'i':
              gui_address = optarg;
              break;
          case 'r':
              port_gui = atoi(optarg);
              break;
          default:
              fatal("Incorrect flag");
          }
  }


  if (!is_player_name_correct()) {
    fatal("Incorrect player name");
  }
  initalize_socks();
  play();
}
