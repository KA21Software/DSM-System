// Statistics and the console (design sections 9.6 and 10). Answers the STATS, PAGE_TABLE and
// ALLOCS requests of other nodes, gathers their answers for the console commands, and runs
// the console thread. Queries are plain control-plane calls, so a node that is down just
// shows up as unreachable in the output.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "alloc.h"
#include "config.h"
#include "control.h"
#include "directory.h"
#include "dsm.h"
#include "membership.h"
#include "memory.h"
#include "names.h"

class Query {
public:
    Query(const Config& cfg, Memory& mem, Membership& ms, Network& data_net, Directory& dir, Allocator& alloc,
          Names& names, ControlPlane& cp)
        : cfg_(cfg), mem_(mem), ms_(ms), data_net_(data_net), dir_(dir), alloc_(alloc), names_(names), cp_(cp) {}
    ~Query() { stop_console(); }

    // Control listener: STATS_REQUEST, PAGE_TABLE_REQUEST, ALLOCS_REQUEST.
    void on_message(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload);

    void local_stats(dsm_stats_t& out);
    int node_stats(node_id_t node, dsm_stats_t& out);
    int global_stats(dsm_stats_t& out);

    // Runs one console command and returns its output. `quit` is set by the quit command.
    std::string run_command(const std::string& line, bool& quit);

    // Reads commands from stdin until EOF or quit. `on_quit` ends the node.
    void start_console(std::function<void()> on_quit);
    void stop_console();

private:
    struct PageRow {
        uint32_t page, version, alloc_id, alloc_order;
        uint8_t local_state;  // page_state_t at the node that answered
        uint8_t dir_state;    // dir_state_t, or kNoDir
        uint8_t busy, lost;
        uint16_t owner;
        uint64_t copyset;
    };
    static constexpr uint8_t kNoDir = 0xFF;

    std::vector<PageRow> collect_pages(uint32_t first, uint32_t count, bool everything);
    int remote_pages(node_id_t node, uint32_t first, uint32_t count, bool everything, std::vector<PageRow>& out);
    std::string cmd_nodes();
    std::string cmd_stats(const std::string& arg);
    std::string cmd_pages_local();
    std::string cmd_pages_global();
    std::string cmd_page(const std::string& arg);
    std::string cmd_allocs();
    std::string cmd_names();
    void console_loop();

    const Config& cfg_;
    Memory& mem_;
    Membership& ms_;
    Network& data_net_;
    Directory& dir_;
    Allocator& alloc_;
    Names& names_;
    ControlPlane& cp_;

    std::thread console_;
    std::atomic<bool> console_running_{false};
    std::function<void()> on_quit_;
};
