// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "svdpi.h"

namespace {

// Maximum payload size (1MB) to prevent OOM from malformed packets.
constexpr size_t MAX_PAYLOAD_SIZE = 1024 * 1024;

// Defines the type of SPI command received over the socket.
enum class CommandType : uint8_t {
  IDLE_CLOCKING = 2,
  PACKED_WRITE = 3,
  V2_WRITE = 8,
  V2_READ = 9,
};

// The command structure sent from the Python client.
struct SpiCommand {
  CommandType type;
  uint32_t addr;   // For reg commands
  uint64_t data;   // For simple writes or expected value
  uint32_t count;  // For bulk commands or wait cycles
} __attribute__((packed));

// The response packet sent back to the Python client.
struct SpiResponse {
  uint64_t data;  // For simple reads
  uint8_t success;
} __attribute__((packed));

// A version of the command for the internal queue that can hold a data payload.
struct QueuedSpiCommand {
  SpiCommand header;
  std::vector<uint8_t> payload;
};

// Thread-safe queues for IPC between the server thread and the simulation
// thread.
std::queue<QueuedSpiCommand> cmd_queue;
std::mutex cmd_mutex;
std::queue<SpiResponse> result_queue;
std::mutex result_mutex;
std::queue<std::vector<uint8_t>> bulk_read_queue;
std::mutex bulk_read_mutex;
std::condition_variable bulk_read_cv;

// Global state for the server thread.
int server_fd = -1;
std::thread server_thread;
std::atomic<bool> shutting_down{false};
std::atomic<bool> reset_done{false};
std::atomic<bool> reset_seen{false};

struct SpiSignalState {
  uint8_t sck;
  uint8_t csb;
  uint8_t mosi;
};

// State for the DPI state machine.
enum SpiFsmState {
  IDLE,
  IDLE_TICKING,
  V2_START,
  V2_HEADER,
  V2_WRITE_DATA,
  V2_READ_WAIT_TOKEN,
  V2_READ_DATA,
  V2_END,
};

struct SpiDpiFsmState {
  SpiFsmState state;
  SpiSignalState signal_state;
  QueuedSpiCommand current_cmd;
  uint8_t data_out = 0;
  uint8_t data_in = 0;
  int bit_count = 0;
  int byte_idx = 0;
  int cycle_wait_count = 0;
  std::vector<uint8_t> bulk_data_buffer;
  bool success = true;

  void init() {
    this->state = IDLE;
    this->signal_state = {0, 1, 0};
    this->bit_count = 0;
    this->byte_idx = 0;
    this->data_out = 0;
    this->data_in = 0;
    this->cycle_wait_count = 0;
    this->bulk_data_buffer.clear();
    this->success = true;
  }
};

void server_loop(int port) {
  struct sockaddr_in address;
  int opt = 1;
  socklen_t addrlen = sizeof(address);

  if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
    perror("socket failed");
    return;
  }

  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
    perror("setsockopt");
    return;
  }
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);

  if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
    perror("bind failed");
    return;
  }
  if (listen(server_fd, 3) < 0) {
    perror("listen");
    return;
  }

  while (!reset_done.load() && !shutting_down.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (shutting_down) return;

  std::cout << "DPI: Server listening on port " << port << std::endl;

  int client_socket;
  if ((client_socket =
           accept(server_fd, (struct sockaddr*)&address, &addrlen)) < 0) {
    if (!shutting_down) perror("accept");
    return;
  }

  while (true) {
    SpiCommand cmd_header;
    size_t header_read = 0;
    while (header_read < sizeof(cmd_header)) {
      int n = read(client_socket,
                   reinterpret_cast<char*>(&cmd_header) + header_read,
                   sizeof(cmd_header) - header_read);
      if (n <= 0) break;
      header_read += n;
    }
    if (header_read < sizeof(cmd_header)) break;

    QueuedSpiCommand q_cmd;
    q_cmd.header = cmd_header;
    bool command_ok = true;

    if (cmd_header.type == CommandType::V2_WRITE) {
      size_t payload_size = (static_cast<size_t>(cmd_header.count) + 1) * 16;
      if (payload_size > MAX_PAYLOAD_SIZE) {
        std::cerr << "DPI: Payload too large (" << payload_size << " bytes)"
                  << std::endl;
        break;
      }
      q_cmd.payload.resize(payload_size);
      size_t total_read = 0;
      while (total_read < payload_size) {
        int n = read(client_socket, q_cmd.payload.data() + total_read,
                     payload_size - total_read);
        if (n <= 0) break;
        total_read += n;
      }
      if (total_read < payload_size) command_ok = false;
    } else if (cmd_header.type == CommandType::PACKED_WRITE) {
      // Legacy packed write for backward compatibility if needed
      size_t payload_size = static_cast<size_t>(cmd_header.count) * 16;
      if (payload_size > MAX_PAYLOAD_SIZE) {
        std::cerr << "DPI: Payload too large (" << payload_size << " bytes)"
                  << std::endl;
        break;
      }
      q_cmd.payload.resize(payload_size);
      size_t total_read = 0;
      while (total_read < payload_size) {
        int n = read(client_socket, q_cmd.payload.data() + total_read,
                     payload_size - total_read);
        if (n <= 0) break;
        total_read += n;
      }
      if (total_read < payload_size) command_ok = false;
    }

    if (!command_ok) break;

    {
      std::lock_guard<std::mutex> lock(cmd_mutex);
      cmd_queue.push(q_cmd);
    }

    while (result_queue.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    SpiResponse response;
    {
      std::lock_guard<std::mutex> lock(result_mutex);
      response = result_queue.front();
      result_queue.pop();
    }
    send(client_socket, &response, sizeof(response), 0);

    if (cmd_header.type == CommandType::V2_READ && response.success) {
      std::vector<uint8_t> read_payload;
      {
        std::unique_lock<std::mutex> lock(bulk_read_mutex);
        bulk_read_cv.wait(lock, [] { return !bulk_read_queue.empty(); });
        read_payload = bulk_read_queue.front();
        bulk_read_queue.pop();
      }
      send(client_socket, read_payload.data(), read_payload.size(), 0);
    }
  }
  close(client_socket);
}

}  // namespace

extern "C" {

struct SpiDpiFsmState* spi_dpi_init() {
  const char* port_str = getenv("SPI_DPI_PORT");
  int port = 5555;
  if (port_str) port = std::stoi(port_str);
  server_thread = std::thread(server_loop, port);
  struct SpiDpiFsmState* ctx = new SpiDpiFsmState();
  ctx->init();
  return ctx;
}

void spi_dpi_close(struct SpiDpiFsmState* ctx) {
  shutting_down = true;
  if (server_fd != -1) shutdown(server_fd, SHUT_RDWR);
  if (server_thread.joinable()) server_thread.join();
  if (ctx) delete ctx;
}

void spi_dpi_reset(struct SpiDpiFsmState* ctx) {
  reset_seen.store(true, std::memory_order_release);
  reset_done.store(false, std::memory_order_release);
  ctx->init();
  {
    std::lock_guard<std::mutex> lock(cmd_mutex);
    std::queue<QueuedSpiCommand> e;
    cmd_queue.swap(e);
  }
  {
    std::lock_guard<std::mutex> lock(result_mutex);
    std::queue<SpiResponse> e;
    result_queue.swap(e);
  }
  {
    std::lock_guard<std::mutex> lock(bulk_read_mutex);
    std::queue<std::vector<uint8_t>> e;
    bulk_read_queue.swap(e);
  }
}

void handle_v2_transaction(unsigned char miso, struct SpiDpiFsmState* ctx) {
  switch (ctx->state) {
    case V2_START:
      ctx->signal_state.csb = 0;
      ctx->signal_state.sck = 0;
      ctx->byte_idx = 0;
      ctx->bit_count = 0;
      ctx->success = true;
      ctx->state = V2_HEADER;
      break;

    case V2_HEADER:
      if (ctx->bit_count == 0 && !ctx->signal_state.sck) {
        uint8_t op = (ctx->current_cmd.header.type == CommandType::V2_READ)
                         ? 0x01
                         : 0x02;
        switch (ctx->byte_idx) {
          case 0:
            ctx->data_out = op;
            break;
          case 1:
            ctx->data_out = (ctx->current_cmd.header.addr >> 24) & 0xFF;
            break;
          case 2:
            ctx->data_out = (ctx->current_cmd.header.addr >> 16) & 0xFF;
            break;
          case 3:
            ctx->data_out = (ctx->current_cmd.header.addr >> 8) & 0xFF;
            break;
          case 4:
            ctx->data_out = ctx->current_cmd.header.addr & 0xFF;
            break;
          case 5:
            ctx->data_out = (ctx->current_cmd.header.count >> 8) & 0xFF;
            break;
          case 6:
            ctx->data_out = ctx->current_cmd.header.count & 0xFF;
            break;
          default:
            ctx->data_out = 0;
            break;
        }
      }
      ctx->signal_state.sck = !ctx->signal_state.sck;
      if (ctx->signal_state.sck) {
        ctx->signal_state.mosi = (ctx->data_out >> (7 - ctx->bit_count)) & 1;
      } else {
        ctx->bit_count++;
        if (ctx->bit_count == 8) {
          ctx->bit_count = 0;
          ctx->byte_idx++;
          if (ctx->byte_idx == 7) {
            ctx->byte_idx = 0;
            if (ctx->current_cmd.header.type == CommandType::V2_WRITE) {
              ctx->state = V2_WRITE_DATA;
            } else {
              ctx->state = V2_READ_WAIT_TOKEN;
              ctx->data_in = 0;
              ctx->cycle_wait_count = 10000;  // Timeout for 0xFE
            }
          }
        }
      }
      break;

    case V2_WRITE_DATA:
      if (ctx->bit_count == 0 && !ctx->signal_state.sck) {
        ctx->data_out = ctx->current_cmd.payload[ctx->byte_idx];
      }
      ctx->signal_state.sck = !ctx->signal_state.sck;
      if (ctx->signal_state.sck) {
        ctx->signal_state.mosi = (ctx->data_out >> (7 - ctx->bit_count)) & 1;
      } else {
        ctx->bit_count++;
        if (ctx->bit_count == 8) {
          ctx->bit_count = 0;
          ctx->byte_idx++;
          if (ctx->byte_idx == ctx->current_cmd.payload.size()) {
            ctx->state = V2_END;
            ctx->cycle_wait_count = 200;  // CDC drain
          }
        }
      }
      break;

    case V2_READ_WAIT_TOKEN:
      ctx->signal_state.sck = !ctx->signal_state.sck;
      if (!ctx->signal_state.sck) {
        ctx->data_in = (ctx->data_in << 1) | miso;
        if (ctx->data_in == 0xFE) {
          ctx->state = V2_READ_DATA;
          ctx->byte_idx = 0;
          ctx->bit_count = 0;
          ctx->data_in = 0;
          ctx->bulk_data_buffer.resize((ctx->current_cmd.header.count + 1) *
                                       16);
        } else if (--ctx->cycle_wait_count <= 0) {
          std::cerr << "DPI: V2_READ timeout at addr 0x" << std::hex
                    << ctx->current_cmd.header.addr << std::endl;
          ctx->success = false;
          ctx->state = V2_END;
          ctx->cycle_wait_count = 200;
        }
      }
      break;

    case V2_READ_DATA:
      ctx->signal_state.sck = !ctx->signal_state.sck;
      if (!ctx->signal_state.sck) {
        ctx->data_in = (ctx->data_in << 1) | miso;
        ctx->bit_count++;
        if (ctx->bit_count == 8) {
          ctx->bulk_data_buffer[ctx->byte_idx++] = ctx->data_in;
          ctx->bit_count = 0;
          ctx->data_in = 0;
          if (ctx->byte_idx == ctx->bulk_data_buffer.size()) {
            ctx->state = V2_END;
            ctx->cycle_wait_count = 200;
          }
        }
      }
      break;

    case V2_END:
      ctx->signal_state.sck = !ctx->signal_state.sck;
      if (--ctx->cycle_wait_count <= 0) {
        ctx->signal_state.sck = 0;
        ctx->signal_state.csb = 1;
        ctx->signal_state.mosi = 0;
        ctx->state = IDLE;
        {
          std::lock_guard<std::mutex> lock(result_mutex);
          result_queue.push({0, (uint8_t)ctx->success});
        }
        if (ctx->current_cmd.header.type == CommandType::V2_READ &&
            ctx->success) {
          {
            std::lock_guard<std::mutex> lock(bulk_read_mutex);
            bulk_read_queue.push(ctx->bulk_data_buffer);
          }
          bulk_read_cv.notify_one();
        }
      }
      break;
    default:
      abort();
  }
}

void spi_dpi_tick(struct SpiDpiFsmState* ctx, unsigned char* sck,
                  unsigned char* csb, unsigned char* mosi, unsigned char miso) {
  if (!reset_done.load(std::memory_order_relaxed) &&
      reset_seen.load(std::memory_order_acquire)) {
    reset_done.store(true, std::memory_order_release);
  }

  if (ctx->state == IDLE) {
    ctx->signal_state = {0, 1, 0};
    std::lock_guard<std::mutex> lock(cmd_mutex);
    if (!cmd_queue.empty()) {
      ctx->current_cmd = cmd_queue.front();
      cmd_queue.pop();
      if (ctx->current_cmd.header.type == CommandType::V2_WRITE ||
          ctx->current_cmd.header.type == CommandType::V2_READ) {
        ctx->state = V2_START;
      } else if (ctx->current_cmd.header.type == CommandType::IDLE_CLOCKING) {
        ctx->state = IDLE_TICKING;
        ctx->cycle_wait_count = ctx->current_cmd.header.count * 2;
      } else {
        // Legacy or unsupported: just ack
        std::lock_guard<std::mutex> lock(result_mutex);
        result_queue.push({0, 1});
        ctx->state = IDLE;
      }
    }
  }

  switch (ctx->state) {
    case IDLE:
      break;
    case IDLE_TICKING:
      ctx->signal_state.sck = !ctx->signal_state.sck;
      if (--ctx->cycle_wait_count <= 0) {
        ctx->signal_state.sck = 0;
        ctx->state = IDLE;
        {
          std::lock_guard<std::mutex> lock(result_mutex);
          result_queue.push({0, 1});
        }
      }
      break;
    case V2_START:
    case V2_HEADER:
    case V2_WRITE_DATA:
    case V2_READ_WAIT_TOKEN:
    case V2_READ_DATA:
    case V2_END:
      handle_v2_transaction(miso, ctx);
      break;
    default:
      break;
  }

  *sck = ctx->signal_state.sck;
  *csb = ctx->signal_state.csb;
  *mosi = ctx->signal_state.mosi;
}

}  // extern "C"
